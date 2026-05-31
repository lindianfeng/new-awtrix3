/**
 * awtrix_io.c — small-peripheral HAL bundle.
 *
 * Holds the C-level helpers that PeripheryManager (C++) calls from its
 * tick loop. Roughly four sub-modules in one translation unit:
 *
 *   1. ADC oneshot — battery + LDR voltage reads
 *        awtrix_adc_init / awtrix_adc_read_battery / awtrix_adc_read_ldr
 *
 *   2. LEDC PWM buzzer — tone generation, volume scaling
 *        awtrix_buzzer_init / _set_volume / _tone / _no_tone
 *
 *   3. I2C sensor probe — auto-detect BMP280 / SHT31 / AHT2x at 0x76/0x44
 *        awtrix_i2c_sensors_init / awtrix_i2c_detect
 *
 *   4. RTTTL parser / scheduler — non-blocking melody player
 *        awtrix_rtttl_play / _stop / _tick / _is_playing
 *
 * The four sub-modules are wedged into one file because they all live
 * on the same physical hardware (the same ESP32-S3 dev board) and share
 * cheap helpers like awtrix_io_now_ms(). Splitting them would multiply
 * the awtrix_periphery CMakeLists boilerplate without a real layering
 * benefit. PeripheryManager owns the schedule:
 *
 *   - awtrix_rtttl_tick() — every frame (62 Hz)
 *   - awtrix_adc_read_battery() / read_ldr() — every 10 s (battery filter)
 *                                              every  1 s (LDR filter)
 *   - awtrix_i2c_detect() — once at boot, cached in PeripheryManager
 */

#include "awtrix_io.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>

#define TAG TAG_IO

/* Forward decls used by RTTTL */
static uint64_t awtrix_io_now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

/* ═══════════════════════════════════════════════════════════════
 *  ADC (IDF v6.0 oneshot API)
 * ═══════════════════════════════════════════════════════════════ */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;

void awtrix_adc_init(void) {
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg);
    adc_oneshot_config_channel(s_adc_handle, LDR_ADC_CHANNEL,     &chan_cfg);
    ESP_LOGI(TAG, "ADC initialized (oneshot, 12bit)");
}

uint16_t awtrix_adc_read_battery(void) {
    int raw = 0;
    adc_oneshot_read(s_adc_handle, BATTERY_ADC_CHANNEL, &raw);
    return (uint16_t)(raw > 0 ? raw : 0);
}

uint16_t awtrix_adc_read_ldr(void) {
    int raw = 0;
    adc_oneshot_read(s_adc_handle, LDR_ADC_CHANNEL, &raw);
    return (uint16_t)(raw > 0 ? raw : 0);
}

/* ═══════════════════════════════════════════════════════════════
 *  FILTERS
 * ═══════════════════════════════════════════════════════════════ */
void awtrix_mf_init(median_filter_t *f, uint16_t *buf, int winsize) {
    memset(buf, 0, winsize * sizeof(uint16_t));
    f->values = buf;
    f->size = winsize;
    f->idx = 0;
    f->count = 0;
}

static int awtrix_cmp_u16(const void *a, const void *b) {
    return (*(uint16_t *)a) - (*(uint16_t *)b);
}

uint16_t awtrix_mf_add(median_filter_t *f, uint16_t val) {
    f->values[f->idx] = val;
    f->idx = (f->idx + 1) % f->size;
    if (f->count < f->size) f->count++;
    /* copy and sort for median */
    uint16_t copy[f->size];
    memcpy(copy, f->values, f->size * sizeof(uint16_t));
    qsort(copy, f->count, sizeof(uint16_t), awtrix_cmp_u16);
    return copy[f->count / 2];
}

void awtrix_af_init(mean_filter_t *f, uint16_t *buf, int winsize) {
    memset(buf, 0, winsize * sizeof(uint16_t));
    f->values = buf;
    f->size = winsize;
    f->idx = 0;
    f->count = 0;
    f->sum = 0;
}

uint16_t awtrix_af_add(mean_filter_t *f, uint16_t val) {
    if (f->count == f->size) {
        f->sum -= f->values[f->idx];
    } else {
        f->count++;
    }
    f->values[f->idx] = val;
    f->sum += val;
    f->idx = (f->idx + 1) % f->size;
    return (uint16_t)(f->sum / f->count);
}

/* ═══════════════════════════════════════════════════════════════
 *  I2C SENSORS (IDF v6.0 i2c_master API)
 * ═══════════════════════════════════════════════════════════════ */
#include "driver/i2c_master.h"

static i2c_master_bus_handle_t  s_i2c_bus = NULL;
static i2c_master_dev_handle_t  s_i2c_dev = NULL;
static sensor_type_t            s_sensor_type = SENSOR_NONE;
static uint8_t                  s_sensor_addr = 0;

/* BME280 calibration coefficients */
static struct {
    uint16_t T1;
    int16_t  T2, T3;
    uint16_t P1;
    int16_t  P2, P3, P4, P5, P6, P7, P8, P9;
    uint8_t  H1;
    int16_t  H2;
    uint8_t  H3;
    int16_t  H4, H5;
    int8_t   H6;
    int32_t  t_fine;  /* updated by each compensate_T() */
    bool     valid;
} s_bme_cal;

static esp_err_t i2c_attach_device(uint8_t addr) {
    if (s_i2c_dev) {
        i2c_master_bus_rm_device(s_i2c_dev);
        s_i2c_dev = NULL;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
}

static esp_err_t i2c_write_reg(uint8_t reg, uint8_t val) {
    if (!s_i2c_dev) return ESP_FAIL;
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_i2c_dev, buf, 2, 100);
}

static esp_err_t i2c_read_regs(uint8_t reg, uint8_t *out, size_t n) {
    if (!s_i2c_dev) return ESP_FAIL;
    return i2c_master_transmit_receive(s_i2c_dev, &reg, 1, out, n, 100);
}

static esp_err_t i2c_read_bytes(uint8_t *out, size_t n) {
    if (!s_i2c_dev) return ESP_FAIL;
    return i2c_master_receive(s_i2c_dev, out, n, 100);
}

static esp_err_t i2c_write_bytes(const uint8_t *buf, size_t n) {
    if (!s_i2c_dev) return ESP_FAIL;
    return i2c_master_transmit(s_i2c_dev, buf, n, 100);
}

/* Read & cache BME280 factory calibration (registers 0x88..0xA1 + 0xE1..0xE7) */
static bool bme280_load_calibration(void) {
    uint8_t b[26];
    if (i2c_read_regs(0x88, b, 26) != ESP_OK) return false;
    s_bme_cal.T1 = (uint16_t)(b[0]  | (b[1]  << 8));
    s_bme_cal.T2 = (int16_t)(b[2]  | (b[3]  << 8));
    s_bme_cal.T3 = (int16_t)(b[4]  | (b[5]  << 8));
    s_bme_cal.P1 = (uint16_t)(b[6]  | (b[7]  << 8));
    s_bme_cal.P2 = (int16_t)(b[8]  | (b[9]  << 8));
    s_bme_cal.P3 = (int16_t)(b[10] | (b[11] << 8));
    s_bme_cal.P4 = (int16_t)(b[12] | (b[13] << 8));
    s_bme_cal.P5 = (int16_t)(b[14] | (b[15] << 8));
    s_bme_cal.P6 = (int16_t)(b[16] | (b[17] << 8));
    s_bme_cal.P7 = (int16_t)(b[18] | (b[19] << 8));
    s_bme_cal.P8 = (int16_t)(b[20] | (b[21] << 8));
    s_bme_cal.P9 = (int16_t)(b[22] | (b[23] << 8));
    s_bme_cal.H1 = b[25];

    uint8_t h[7];
    if (i2c_read_regs(0xE1, h, 7) != ESP_OK) return false;
    s_bme_cal.H2 = (int16_t)(h[0] | (h[1] << 8));
    s_bme_cal.H3 = h[2];
    s_bme_cal.H4 = (int16_t)((h[3] << 4) | (h[4] & 0x0F));
    s_bme_cal.H5 = (int16_t)((h[5] << 4) | (h[4] >> 4));
    s_bme_cal.H6 = (int8_t)h[6];
    s_bme_cal.valid = true;
    return true;
}

void awtrix_i2c_sensors_init(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port           = I2C_NUM_0,
        .sda_io_num         = I2C_SDA_PIN,
        .scl_io_num         = I2C_SCL_PIN,
        .clk_source         = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt  = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));
    s_sensor_type = awtrix_i2c_detect_sensor();
    ESP_LOGI(TAG, "I2C sensor detected: type=%d addr=0x%02X", (int)s_sensor_type, s_sensor_addr);
}

sensor_type_t awtrix_i2c_detect_sensor(void) {
    s_sensor_type = SENSOR_NONE;
    s_sensor_addr = 0;

    /* 1) BME280 / BMP280 share 0x76/0x77; distinguished by chip_id @0xD0 */
    const uint8_t bmp_addrs[] = { 0x76, 0x77 };
    for (int i = 0; i < 2; i++) {
        if (i2c_master_probe(s_i2c_bus, bmp_addrs[i], pdMS_TO_TICKS(50)) != ESP_OK) continue;
        if (i2c_attach_device(bmp_addrs[i]) != ESP_OK) continue;
        uint8_t id = 0;
        if (i2c_read_regs(0xD0, &id, 1) == ESP_OK) {
            if (id == 0x60) {
                if (bme280_load_calibration()) {
                    s_sensor_type = SENSOR_BME280;
                    s_sensor_addr = bmp_addrs[i];
                    return s_sensor_type;
                }
            } else if (id == 0x58) {
                /* BMP280 — temperature only; reuse the same calibration loader
                 * because P/T parts overlap; humidity coeffs simply unused. */
                if (bme280_load_calibration()) {
                    s_sensor_type = SENSOR_BMP280;
                    s_sensor_addr = bmp_addrs[i];
                    return s_sensor_type;
                }
            }
        }
    }

    /* 2) SHT31 @ 0x44 */
    if (i2c_master_probe(s_i2c_bus, 0x44, pdMS_TO_TICKS(50)) == ESP_OK) {
        if (i2c_attach_device(0x44) == ESP_OK) {
            s_sensor_type = SENSOR_SHT31;
            s_sensor_addr = 0x44;
            return s_sensor_type;
        }
    }

    /* 3) HTU21DF @ 0x40 */
    if (i2c_master_probe(s_i2c_bus, 0x40, pdMS_TO_TICKS(50)) == ESP_OK) {
        if (i2c_attach_device(0x40) == ESP_OK) {
            s_sensor_type = SENSOR_HTU21DF;
            s_sensor_addr = 0x40;
            return s_sensor_type;
        }
    }

    if (s_i2c_dev) { i2c_master_bus_rm_device(s_i2c_dev); s_i2c_dev = NULL; }
    return SENSOR_NONE;
}

/* ── BME280 / BMP280 compensation (Bosch reference impl, fixed-point) ── */
static float bme280_compensate_T(int32_t adc_T) {
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)s_bme_cal.T1 << 1))) * ((int32_t)s_bme_cal.T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)s_bme_cal.T1)) *
                      ((adc_T >> 4) - ((int32_t)s_bme_cal.T1))) >> 12) *
                    ((int32_t)s_bme_cal.T3)) >> 14;
    s_bme_cal.t_fine = var1 + var2;
    int32_t T = (s_bme_cal.t_fine * 5 + 128) >> 8;
    return (float)T / 100.0f;
}

static float bme280_compensate_H(int32_t adc_H) {
    int32_t v = s_bme_cal.t_fine - 76800;
    v = (((((adc_H << 14) - (((int32_t)s_bme_cal.H4) << 20) - (((int32_t)s_bme_cal.H5) * v)) +
          16384) >> 15) *
         (((((((v * ((int32_t)s_bme_cal.H6)) >> 10) *
              (((v * ((int32_t)s_bme_cal.H3)) >> 11) + 32768)) >> 10) +
            2097152) *
               ((int32_t)s_bme_cal.H2) +
           8192) >> 14));
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * ((int32_t)s_bme_cal.H1) ) >> 4);
    if (v < 0)         v = 0;
    if (v > 419430400) v = 419430400;
    return (float)(v >> 12) / 1024.0f;
}

static esp_err_t bme280_force_measure(uint8_t raw[8], bool with_hum) {
    if (!s_bme_cal.valid) return ESP_FAIL;
    if (with_hum) {
        if (i2c_write_reg(0xF2, 0x01) != ESP_OK) return ESP_FAIL;     /* ctrl_hum: x1 */
    }
    if (i2c_write_reg(0xF4, 0x25) != ESP_OK) return ESP_FAIL;          /* ctrl_meas: T x1, P x1, forced */
    vTaskDelay(pdMS_TO_TICKS(10));
    return i2c_read_regs(0xF7, raw, 8);
}

bool awtrix_i2c_read_bme280(float *temp, float *hum) {
    if (s_sensor_type != SENSOR_BME280 || s_sensor_addr == 0) return false;
    uint8_t r[8];
    if (bme280_force_measure(r, true) != ESP_OK) return false;
    int32_t adc_T = ((int32_t)r[3] << 12) | ((int32_t)r[4] << 4) | (r[5] >> 4);
    int32_t adc_H = ((int32_t)r[6] << 8)  |  (int32_t)r[7];
    if (temp) *temp = bme280_compensate_T(adc_T);
    if (hum)  *hum  = bme280_compensate_H(adc_H);
    return true;
}

bool awtrix_i2c_read_bmp280(float *temp) {
    if (s_sensor_type != SENSOR_BMP280 || s_sensor_addr == 0) return false;
    uint8_t r[8];
    if (bme280_force_measure(r, false) != ESP_OK) return false;
    int32_t adc_T = ((int32_t)r[3] << 12) | ((int32_t)r[4] << 4) | (r[5] >> 4);
    if (temp) *temp = bme280_compensate_T(adc_T);
    return true;
}

bool awtrix_i2c_read_htu21df(float *temp, float *hum) {
    if (s_sensor_type != SENSOR_HTU21DF || s_sensor_addr == 0) return false;
    uint8_t cmd, r[3];
    /* trigger temp (no hold) */
    cmd = 0xF3;
    if (i2c_write_bytes(&cmd, 1) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(55));
    if (i2c_read_bytes(r, 3) != ESP_OK) return false;
    uint16_t rawT = ((uint16_t)r[0] << 8) | (r[1] & 0xFC);
    if (temp) *temp = -46.85f + 175.72f * (float)rawT / 65536.0f;

    cmd = 0xF5;
    if (i2c_write_bytes(&cmd, 1) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (i2c_read_bytes(r, 3) != ESP_OK) return false;
    uint16_t rawH = ((uint16_t)r[0] << 8) | (r[1] & 0xFC);
    if (hum) *hum = -6.0f + 125.0f * (float)rawH / 65536.0f;
    return true;
}

bool awtrix_i2c_read_sht31(float *temp, float *hum) {
    if (s_sensor_type != SENSOR_SHT31 || s_sensor_addr == 0) return false;
    /* high-repeatability single shot: 0x2400 */
    const uint8_t cmd[2] = { 0x24, 0x00 };
    if (i2c_write_bytes(cmd, 2) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t r[6];
    if (i2c_read_bytes(r, 6) != ESP_OK) return false;
    uint16_t rawT = ((uint16_t)r[0] << 8) | r[1];
    uint16_t rawH = ((uint16_t)r[3] << 8) | r[4];
    if (temp) *temp = -45.0f + 175.0f * (float)rawT / 65535.0f;
    if (hum)  *hum  =  100.0f * (float)rawH / 65535.0f;
    return true;
}

/* ═══════════════════════════════════════════════════════════════
 *  BUZZER (LEDC PWM)
 * ═══════════════════════════════════════════════════════════════ */
#include "driver/ledc.h"

static uint8_t s_buzzer_vol_pct = 50;  /* 0-100 */

void awtrix_buzzer_init(void) {
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);
    ledc_channel_config_t ch = {
        .gpio_num = BUZZER_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch);
    ESP_LOGI(TAG, "Buzzer on GPIO%d", BUZZER_PIN);
}

void awtrix_buzzer_tone(uint16_t freq_hz) {
    if (freq_hz < 20) { awtrix_buzzer_no_tone(); return; }
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq_hz);
    uint32_t duty = (1024 * s_buzzer_vol_pct) / 100 / 2;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void awtrix_buzzer_no_tone(void) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void awtrix_buzzer_set_volume(uint8_t vol_0_30) {
    s_buzzer_vol_pct = (vol_0_30 * 100) / 30;
    if (s_buzzer_vol_pct > 100) s_buzzer_vol_pct = 100;
}

/* ═══════════════════════════════════════════════════════════════
 *  RTTTL PLAYER (minimal in-place implementation)
 * ═══════════════════════════════════════════════════════════════ */

/* Note frequencies for octave 4 (c4=middle C). Other octaves are scaled by
 * powers of two. Index: 0=c, 1=c#, 2=d, 3=d#, 4=e, 5=f, 6=f#, 7=g, 8=g#,
 * 9=a, 10=a#, 11=b. Index 12 = pause. */
static const uint16_t s_note_freq_oct4[13] = {
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494, 0
};

#define RTTTL_MAX_NOTES 256

typedef struct {
    uint16_t freq;
    uint16_t duration_ms;
} rtttl_note_t;

static rtttl_note_t s_rtttl_notes[RTTTL_MAX_NOTES];
static int      s_rtttl_count   = 0;
static int      s_rtttl_index   = 0;
static uint64_t s_rtttl_next_ms = 0;
static bool     s_rtttl_active  = false;

static int rtttl_note_to_index(char c) {
    switch (c) {
        case 'c': return 0;
        case 'd': return 2;
        case 'e': return 4;
        case 'f': return 5;
        case 'g': return 7;
        case 'a': return 9;
        case 'b': return 11;
        case 'p': return 12;  /* pause */
        default:  return -1;
    }
}

static uint16_t rtttl_freq_for(int note_idx, int octave) {
    if (note_idx == 12 || note_idx < 0 || note_idx > 12) return 0;
    int oct = octave;
    if (oct < 4) {
        return s_note_freq_oct4[note_idx] >> (4 - oct);
    } else if (oct > 4) {
        return s_note_freq_oct4[note_idx] << (oct - 4);
    }
    return s_note_freq_oct4[note_idx];
}

bool awtrix_rtttl_play(const char *rtttl_string) {
    if (!rtttl_string || !*rtttl_string) return false;

    /* Reset state */
    awtrix_rtttl_stop();
    s_rtttl_count = 0;
    s_rtttl_index = 0;

    const char *p = rtttl_string;

    /* 1) skip "name:" header (everything up to first ':') */
    const char *colon1 = strchr(p, ':');
    if (!colon1) return false;
    p = colon1 + 1;

    /* 2) defaults section "d=N,o=N,b=N" up to next ':' */
    int def_dur = 4, def_oct = 6, bpm = 63;
    const char *colon2 = strchr(p, ':');
    if (!colon2) return false;
    while (p < colon2) {
        while (p < colon2 && (*p == ' ' || *p == ',')) p++;
        if (p >= colon2) break;
        char key = (char)tolower((unsigned char)*p);
        p++;
        if (p < colon2 && *p == '=') p++;
        int v = (int)strtol(p, (char **)&p, 10);
        if (key == 'd' && v > 0) def_dur = v;
        else if (key == 'o' && v > 0) def_oct = v;
        else if (key == 'b' && v > 0) bpm    = v;
    }
    p = colon2 + 1;

    /* whole-note duration in ms = 60000 / bpm * 4 */
    long whole_ms = (long)(60000L * 4L / bpm);

    /* 3) note list */
    while (*p && s_rtttl_count < RTTTL_MAX_NOTES) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        int dur = def_dur;
        if (isdigit((unsigned char)*p)) {
            dur = (int)strtol(p, (char **)&p, 10);
            if (dur <= 0) dur = def_dur;
        }

        int note_idx = rtttl_note_to_index((char)tolower((unsigned char)*p));
        if (note_idx < 0) { p++; continue; }
        p++;

        /* optional sharp '#' */
        if (*p == '#') { if (note_idx < 12) note_idx++; p++; }

        /* optional dotted '.' (RTTTL allows before or after octave) */
        bool dotted = false;
        if (*p == '.') { dotted = true; p++; }

        int oct = def_oct;
        if (isdigit((unsigned char)*p)) {
            oct = *p - '0';
            p++;
        }

        if (*p == '.') { dotted = true; p++; }

        long ms = whole_ms / dur;
        if (dotted) ms = ms * 3 / 2;

        s_rtttl_notes[s_rtttl_count].freq        = rtttl_freq_for(note_idx, oct);
        s_rtttl_notes[s_rtttl_count].duration_ms = (uint16_t)(ms > 0 ? ms : 0);
        s_rtttl_count++;

        /* advance past trailing comma */
        while (*p == ' ') p++;
        if (*p == ',') p++;
    }

    if (s_rtttl_count == 0) return false;

    /* Begin playback: arm first note immediately on next tick. */
    s_rtttl_active  = true;
    s_rtttl_index   = 0;
    s_rtttl_next_ms = 0;
    ESP_LOGI(TAG, "RTTTL: parsed %d notes, bpm=%d, def_dur=%d, def_oct=%d",
             s_rtttl_count, bpm, def_dur, def_oct);
    return true;
}

bool awtrix_rtttl_play_file(const char *path) {
    if (!path || !*path) return false;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        ESP_LOGW(TAG, "RTTTL file not found: %s", path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 4096) { fclose(fp); return false; }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(fp); return false; }
    size_t n = fread(buf, 1, sz, fp);
    fclose(fp);
    buf[n] = 0;
    bool ok = awtrix_rtttl_play(buf);
    free(buf);
    return ok;
}

void awtrix_rtttl_tick(void) {
    if (!s_rtttl_active) return;
    uint64_t now = awtrix_io_now_ms();
    if (now < s_rtttl_next_ms) return;

    if (s_rtttl_index >= s_rtttl_count) {
        awtrix_rtttl_stop();
        return;
    }

    rtttl_note_t *n = &s_rtttl_notes[s_rtttl_index++];
    if (n->freq == 0) {
        awtrix_buzzer_no_tone();
    } else {
        awtrix_buzzer_tone(n->freq);
    }
    /* Schedule next note slightly before this one ends so the buzzer briefly
     * silences between notes (~10 ms gap), giving cleaner articulation. */
    uint16_t play = n->duration_ms;
    uint16_t gap  = 10;
    if (play > gap + 5) play -= gap;
    s_rtttl_next_ms = now + play;
}

void awtrix_rtttl_stop(void) {
    s_rtttl_active = false;
    s_rtttl_index  = 0;
    s_rtttl_count  = 0;
    awtrix_buzzer_no_tone();
}

bool awtrix_rtttl_is_playing(void) { return s_rtttl_active; }

