#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "awtrix_hal.h"
#include <stdint.h>
#include <stdbool.h>

/* ── ADC subsystem ──────────────────────────────────────────── */
void     awtrix_adc_init(void);
uint16_t awtrix_adc_read_battery(void);    /* returns raw ADC value */
uint16_t awtrix_adc_read_ldr(void);        /* returns raw ADC value */

/* ── Median + Mean filters (replaces MedianFilterLib / MeanFilterLib) ── */
typedef struct {
    uint16_t *values;
    int       size;
    int       idx;
    int       count;
} median_filter_t;

typedef struct {
    uint16_t *values;
    int       size;
    int       idx;
    int       count;
    uint32_t  sum;
} mean_filter_t;

void     awtrix_mf_init(median_filter_t *f, uint16_t *buf, int winsize);
uint16_t awtrix_mf_add(median_filter_t *f, uint16_t val);
void     awtrix_af_init(mean_filter_t *f, uint16_t *buf, int winsize);
uint16_t awtrix_af_add(mean_filter_t *f, uint16_t val);

/* ── I2C sensors ────────────────────────────────────────────── */
typedef enum {
    SENSOR_NONE = 0,
    SENSOR_BME280,
    SENSOR_BMP280,
    SENSOR_HTU21DF,
    SENSOR_SHT31,
} sensor_type_t;

void     awtrix_i2c_sensors_init(void);
sensor_type_t awtrix_i2c_detect_sensor(void);
bool     awtrix_i2c_read_bme280(float *temp, float *hum);
bool     awtrix_i2c_read_bmp280(float *temp);
bool     awtrix_i2c_read_htu21df(float *temp, float *hum);
bool     awtrix_i2c_read_sht31(float *temp, float *hum);

/* ── Buzzer (LEDC PWM) ──────────────────────────────────────── */
void awtrix_buzzer_init(void);
void awtrix_buzzer_tone(uint16_t freq_hz);
void awtrix_buzzer_no_tone(void);
void awtrix_buzzer_set_volume(uint8_t vol_0_30);  /* 0-30 scale → PWM duty */

/* ── RTTTL player ───────────────────────────────────────────── */
bool     awtrix_rtttl_play(const char *rtttl_string);
bool     awtrix_rtttl_play_file(const char *path);
void     awtrix_rtttl_tick(void);     /* call from main loop every ~10ms */
void     awtrix_rtttl_stop(void);
bool     awtrix_rtttl_is_playing(void);


#ifdef __cplusplus
}
#endif