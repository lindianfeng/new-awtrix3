#include "awtrix_periphery.h"
#include "awtrix_globals.h"
#include "awtrix_events.h"   /* EVENTS.setBrightness(...) — replaces direct DisplayManager.h dep */
#include <cmath>

#include "cJSON.h"
#include "esp_timer.h"

#define TAG TAG_IO

static uint64_t _now(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

/* ═══════════════════ PeripheryManager ════════════════════════ */

/* ── iot_button callback ──────────────────────────────────── */
static void _button_event_cb(void *arg, void *usr_data) {
    int idx = (intptr_t)usr_data;
    button_handle_t btn = (button_handle_t)arg;
    button_event_t evt = iot_button_get_event(btn);
    if (!btn || idx < 0 || idx >= BTN_COUNT) return;

    auto &peri = PeripheryManager::get();
    switch (evt) {
        case BUTTON_SINGLE_CLICK:      peri.enqueueBtnEvent(idx, BTN_EVENT_PRESSED);        break;
        case BUTTON_DOUBLE_CLICK:      peri.enqueueBtnEvent(idx, BTN_EVENT_DOUBLE_PRESS);   break;
        case BUTTON_LONG_PRESS_START:  peri.enqueueBtnEvent(idx, BTN_EVENT_LONG_PRESS);     break;
        case BUTTON_LONG_PRESS_HOLD:
            peri.enqueueBtnEvent(idx, BTN_EVENT_VERY_LONG_PRESS);
            break;
        case BUTTON_PRESS_UP:
            peri.clearVeryLongFired(idx);
            break;
        default: break;
    }
}

void PeripheryManager::setup() {
    auto &cfg = CONFIG;
    m_bootTime = _now();

    /* buttons — using espressif/button component */
    static const struct { int gpio; } btn_cfg[] = {
        { BUTTON_LEFT_PIN },
        { BUTTON_SELECT_PIN },
        { BUTTON_RIGHT_PIN },
        { BUTTON_RESET_PIN },
    };
    for (int i = 0; i < BTN_COUNT; i++) {
        button_config_t cfg = {
            .long_press_time = 1000,        /* 1s → LONG_PRESS_START */
            .short_press_time = 50,         /* debounce floor */
        };
        button_gpio_config_t gpio_cfg = {
            .gpio_num = btn_cfg[i].gpio,
            .active_level = 0,              /* pulled-up, GND = active */
            .enable_power_save = false,
            .disable_pull = false,
        };
        m_btns[i] = NULL;
        if (iot_button_new_gpio_device(&cfg, &gpio_cfg, &m_btns[i]) != ESP_OK) {
            m_btns[i] = NULL;
        }
        if (m_btns[i]) {
            iot_button_register_cb(m_btns[i], BUTTON_SINGLE_CLICK,     NULL, _button_event_cb, (void *)(intptr_t)i);
            iot_button_register_cb(m_btns[i], BUTTON_DOUBLE_CLICK,     NULL, _button_event_cb, (void *)(intptr_t)i);
            iot_button_register_cb(m_btns[i], BUTTON_LONG_PRESS_START, NULL, _button_event_cb, (void *)(intptr_t)i);
            iot_button_register_cb(m_btns[i], BUTTON_LONG_PRESS_HOLD,  NULL, _button_event_cb, (void *)(intptr_t)i);
            iot_button_register_cb(m_btns[i], BUTTON_PRESS_UP,         NULL, _button_event_cb, (void *)(intptr_t)i);
        }
    }

    /* ADC */
    awtrix_adc_init();

    /* filters */
    awtrix_mf_init(&m_mfBat, m_medBatBuf, 7);
    awtrix_af_init(&m_afBat, m_meanBatBuf, 7);
    awtrix_mf_init(&m_mfLdr, m_medLdrBuf, 7);
    awtrix_af_init(&m_afLdr, m_meanLdrBuf, 7);

    /* I2C sensors */
    awtrix_i2c_sensors_init();
    m_sensorType = awtrix_i2c_detect_sensor();
    cfg.tempSensorType = (uint8_t)m_sensorType;

    /* buzzer */
    awtrix_buzzer_init();

    /* Note: button left/right swap based on rotate_screen/swap_buttons
       is handled at the app level via BTN_LEFT/BTN_RIGHT index mapping. */

    ESP_LOGI(TAG, "Periphery setup done (sensor=%d)", (int)m_sensorType);
}

void PeripheryManager::tick() {
    auto &cfg = CONFIG;
    uint64_t now = _now();

    /* ── drain button events (produced by iot_button callbacks) ── */
    for (int i = 0; i < BTN_COUNT; i++) {
        portENTER_CRITICAL(&m_btnLock);
        btn_event_t evt = m_pendingBtnEvent[i];
        m_pendingBtnEvent[i] = BTN_EVENT_NONE;
        portEXIT_CRITICAL(&m_btnLock);
        if (evt != BTN_EVENT_NONE) {
            ESP_LOGD(TAG, "Button %d event %d", i, (int)evt);
            if (onButtonEvent) onButtonEvent(i, evt);
        }
    }

    /* ── battery + temperature/humidity (10s interval) ────── */
    if (now - m_lastBatTempHum >= 10000) {
        m_lastBatTempHum = now;

        uint16_t rawBat = awtrix_adc_read_battery();
        if (rawBat > 100 && rawBat < 1000) {
            uint16_t filtered = awtrix_af_add(&m_afBat, awtrix_mf_add(&m_mfBat, rawBat));
            cfg.batteryRaw = filtered;
            cfg.batteryPercent = (uint8_t)fmax(0, fmin(100,
                (int)(((float)(filtered - cfg.minBattery) /
                       (float)(cfg.maxBattery - cfg.minBattery)) * 100.0f)));
            cfg.sensorsStable = true;
        }

        if (cfg.sensorReading) {
            switch (m_sensorType) {
            case SENSOR_BME280:
                awtrix_i2c_read_bme280(&cfg.currentTemp, &cfg.currentHum); break;
            case SENSOR_BMP280:
                awtrix_i2c_read_bmp280(&cfg.currentTemp); break;
            case SENSOR_HTU21DF:
                awtrix_i2c_read_htu21df(&cfg.currentTemp, &cfg.currentHum); break;
            case SENSOR_SHT31:
                awtrix_i2c_read_sht31(&cfg.currentTemp, &cfg.currentHum); break;
            default: break;
            }
            cfg.currentTemp += cfg.tempOffset;
            cfg.currentHum  += cfg.humOffset;
        }
    }

    /* ── LDR / auto-brightness (100ms interval) ───────────── */
    if (now - m_lastLdr >= 100) {
        m_lastLdr = now;

        uint16_t rawLdr = awtrix_adc_read_ldr();
        if (cfg.ldr_on_ground) rawLdr = 4095 - rawLdr;
        uint16_t ldrFiltered = awtrix_af_add(&m_afLdr, awtrix_mf_add(&m_mfLdr, rawLdr));
        cfg.ldrRaw = ldrFiltered;

        /* simple lux approximation (raw normalized to [0..1]) */
        float norm = (float)ldrFiltered / 4095.0f;
        cfg.currentLux = norm * 1000.0f;

        if (cfg.auto_brightness && !cfg.matrix_off) {
            /* Mirrors original Arduino formula in PeripheryManager.cpp:
             *   bp = (LDR_RAW * LDR_FACTOR) / FULL_SCALE * 100
             *   bp = pow(bp, gamma) / pow(100, gamma - 1)
             *   BRIGHTNESS = map(bp, 0, 100, MIN, MAX)
             * Original used 1023 (10-bit Arduino ADC); ESP32-S3 uses 12-bit 4095.
             */
            float bp = ((float)ldrFiltered * cfg.ldr_factor) / 4095.0f * 100.0f;
            if (bp < 0.0f)   bp = 0.0f;
            if (bp > 100.0f) bp = 100.0f;
            bp = powf(bp, cfg.ldr_gamma) / powf(100.0f, cfg.ldr_gamma - 1.0f);
            int span = (int)cfg.max_brightness - (int)cfg.min_brightness;
            int bri  = (int)((bp / 100.0f) * (float)span) + (int)cfg.min_brightness;
            if (bri < (int)cfg.min_brightness) bri = cfg.min_brightness;
            if (bri > (int)cfg.max_brightness) bri = cfg.max_brightness;
            cfg.brightness = bri;
            EVENTS.setBrightness(bri);   /* dispatch via event bus, decoupled from awtrix_core */
        }
    }
}

/* ── audio ───────────────────────────────────────────────────── */
void PeripheryManager::playBootSound() {
    auto &cfg = CONFIG;
    if (!cfg.soundActive) return;
    if (!cfg.bootSound.empty()) {
        awtrix_rtttl_play(cfg.bootSound.c_str());
    } else {
        /* default short beep sequence */
        awtrix_buzzer_set_volume(cfg.soundVolume);
        awtrix_buzzer_tone(523); vTaskDelay(pdMS_TO_TICKS(150));
        awtrix_buzzer_tone(659); vTaskDelay(pdMS_TO_TICKS(150));
        awtrix_buzzer_tone(784); vTaskDelay(pdMS_TO_TICKS(150));
        awtrix_buzzer_no_tone();
    }
}

bool PeripheryManager::playFromFile(const char *filename) {
    auto &cfg = CONFIG;
    if (!cfg.soundActive) return false;
    /* RTTTL file from SPIFFS */
    char path[64];
    snprintf(path, sizeof(path), "/spiffs/MELODIES/%s.txt", filename);
    return awtrix_rtttl_play_file(path);
}

bool PeripheryManager::playRTTTL(const char *rtttl) {
    auto &cfg = CONFIG;
    if (!cfg.soundActive) return false;
    awtrix_buzzer_set_volume(cfg.soundVolume);
    return awtrix_rtttl_play(rtttl);
}

bool PeripheryManager::parseSound(const char *json) {
    /* if it looks like JSON, extract "sound" key; else treat as filename */
    if (strchr(json, '{')) {
        cJSON *doc = cJSON_Parse(json);
        if (!doc) return playFromFile(json);
        cJSON *snd = cJSON_GetObjectItem(doc, "sound");
        bool ok = snd && snd->valuestring ? playFromFile(snd->valuestring) : false;
        cJSON_Delete(doc);
        return ok;
    }
    return playFromFile(json);
}

bool PeripheryManager::isPlaying() {
    return awtrix_rtttl_is_playing();
}

void PeripheryManager::stopSound() {
    awtrix_rtttl_stop();
}

void PeripheryManager::setVolume(uint8_t vol_0_30) {
    awtrix_buzzer_set_volume(vol_0_30);
    CONFIG.soundVolume = vol_0_30;
}

void PeripheryManager::r2d2(const char *msg) {
    awtrix_buzzer_set_volume(CONFIG.soundVolume);
    for (const char *p = msg; *p; p++) {
        if (isalpha((int)*p)) {
            uint16_t f = (toupper(*p) - 'A' + 1) * 50;
            awtrix_buzzer_tone(f);
            vTaskDelay(pdMS_TO_TICKS(60)); /* ~50 baud */
        }
    }
    awtrix_buzzer_no_tone();
}

uint64_t PeripheryManager::readUptimeSec() {
    return (_now() - m_bootTime) / 1000;
}