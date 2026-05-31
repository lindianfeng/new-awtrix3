#include "awtrix_periphery.h"
#include "awtrix_globals.h"
#include "awtrix_events.h"   /* EVENTS.{rtttl,r2d2,...} — immediate-action only after E1 */
#include "awtrix_command_bus.h" /* E1: post SetBrightness command directly */
#include "awtrix_dfplayer.h" /* Pack E: optional UART2 MP3 player, gated by CONFIG.dfplayerActive */
#include <cmath>

#include "cJSON.h"
#include "esp_timer.h"

/* iot_button SDK headers — implementation-only, deliberately not exposed
 * via awtrix_periphery.h (see header comment "Header hygiene (round 3)"). */
#include "iot_button.h"
#include "button_gpio.h"

#define TAG TAG_IO

static uint64_t _now(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

/* ═══════════════════ PeripheryManager ════════════════════════ */

/* ── Out-of-line lock-protected button-event helpers ─────────
 * These used to be inline in the header so iot_button SDK callbacks could
 * call them directly. The header no longer exposes FreeRTOS macros for
 * non-trivial use, so we moved the implementations down here. The bodies
 * are tiny — the critical section is what matters, not call overhead. */
void PeripheryManager::enqueueBtnEvent(int idx, btn_event_t evt) {
    if (idx < 0 || idx >= BTN_COUNT) return;
    portENTER_CRITICAL(&m_btnLock);
    m_pendingBtnEvent[idx] = evt;
    portEXIT_CRITICAL(&m_btnLock);
}

void PeripheryManager::clearVeryLongFired(int idx) {
    if (idx < 0 || idx >= BTN_COUNT) return;
    portENTER_CRITICAL(&m_btnLock);
    m_veryLongFired[idx] = false;
    portEXIT_CRITICAL(&m_btnLock);
}

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
        /* m_btns[i] is a void* in the header so external compilation units
         * do not need to see button_handle_t. Cast through a local typed
         * pointer so iot_button_new_gpio_device can write into it. */
        button_handle_t handle = nullptr;
        if (iot_button_new_gpio_device(&cfg, &gpio_cfg, &handle) != ESP_OK) {
            handle = nullptr;
        }
        m_btns[i] = handle;
        if (handle) {
            iot_button_register_cb(handle, BUTTON_SINGLE_CLICK,     NULL, _button_event_cb, (void *)(intptr_t)i);
            iot_button_register_cb(handle, BUTTON_DOUBLE_CLICK,     NULL, _button_event_cb, (void *)(intptr_t)i);
            iot_button_register_cb(handle, BUTTON_LONG_PRESS_START, NULL, _button_event_cb, (void *)(intptr_t)i);
            iot_button_register_cb(handle, BUTTON_LONG_PRESS_HOLD,  NULL, _button_event_cb, (void *)(intptr_t)i);
            iot_button_register_cb(handle, BUTTON_PRESS_UP,         NULL, _button_event_cb, (void *)(intptr_t)i);
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

    /* buzzer (always initialized so it can act as fallback) */
    awtrix_buzzer_init();

    /* Pack E: DFPlayer Mini MP3 on UART2, gated by CONFIG.dfplayerActive.
     * Mirrors original PeripheryManager.cpp which kept DFPlayer cold-booted
     * unless DFPLAYER_ACTIVE was true (UART pins are otherwise free). */
    if (cfg.dfplayerActive) {
        if (awtrix_dfp_init()) {
            awtrix_dfp_set_volume(cfg.soundVolume);
        } else {
            /* Bug 3: actively clear the flag so the routing checks in
             * playFromFile/playRTTTL/parseSound/setVolume/r2d2/stopSound
             * fall through to the buzzer path. Otherwise every sound call
             * silently sends UART commands to a non-existent device and
             * the user gets total silence with no visible error. */
            ESP_LOGE(TAG, "DFPlayer init failed — disabling dfplayerActive, falling back to buzzer");
            AwtrixConfig::Guard guard(CONFIG);
            CONFIG.dfplayerActive = false;
        }
    }

    /* Note: button left/right swap based on rotate_screen/swap_buttons
       is handled at the app level via BTN_LEFT/BTN_RIGHT index mapping. */

    ESP_LOGI(TAG, "Periphery setup done (sensor=%d, dfp=%d)",
             (int)m_sensorType, cfg.dfplayerActive ? 1 : 0);
}

void PeripheryManager::tick() {
    auto &cfg = CONFIG;
    uint64_t now = _now();

    /* RTTTL playback is owned by the periphery layer, so drive its tick from
     * here instead of asking main to know about it. The function is a no-op
     * when nothing is playing, so calling it every frame is essentially free
     * (one bool check) and keeps awtrix_io.h out of app_main's include set. */
    awtrix_rtttl_tick();

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
        int brightnessEvent = -1;
        {
            AwtrixConfig::Guard guard(cfg);
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
                brightnessEvent = bri;
            }
        }
        if (brightnessEvent >= 0) {
            /* E1: post directly to the command bus. DisplayManager is the
             * single consumer that applies the new brightness; routing it
             * via EventBus + main lambda + command_bus was three indirections
             * for one queue entry. */
            AwtrixCommand command;
            command.type = AwtrixCommandType::SetBrightness;
            command.source = AWTRIX_COMMAND_SOURCE_PERIPHERY;
            command.payload = std::to_string(brightnessEvent);
            awtrix_command_bus_post(command, 0);
        }
    }
}

/* ── audio ───────────────────────────────────────────────────── */
/* Pack E selection policy (mirrors original PeripheryManager.cpp):
 *   dfplayerActive=true  → UART2 DFPlayer Mini, RTTTL is not synthesised
 *                          (DFPlayer plays MP3 files indexed 1..N off the SD
 *                          card; we map filename "N" / "0001.mp3" → index N).
 *   dfplayerActive=false → original on-board buzzer + RTTTL parser path. */

static uint16_t dfp_index_from_name(const char *name) {
    if (!name || !*name) return 0;
    /* accept "5", "0005", "0005.mp3", "track5" → all yield index 5 */
    uint16_t n = 0;
    for (const char *p = name; *p; ++p) {
        if (*p >= '0' && *p <= '9') {
            n = n * 10 + (uint16_t)(*p - '0');
            if (n > 9999) break;
        } else if (n > 0) {
            break;     /* stop at first non-digit after we already collected digits */
        }
    }
    return n;
}

void PeripheryManager::playBootSound() {
    auto &cfg = CONFIG;
    if (!cfg.soundActive) return;
    if (cfg.dfplayerActive) {
        if (!cfg.bootSound.empty()) {
            uint16_t idx = dfp_index_from_name(cfg.bootSound.c_str());
            if (idx > 0) awtrix_dfp_play_file_number(idx);
        } else {
            awtrix_dfp_play_file_number(1);   /* convention: track 1 = boot sound */
        }
        return;
    }
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
    if (cfg.dfplayerActive) {
        uint16_t idx = dfp_index_from_name(filename);
        if (idx == 0) return false;
        awtrix_dfp_play_file_number(idx);
        return true;
    }
    /* RTTTL file from SPIFFS */
    char path[64];
    snprintf(path, sizeof(path), "/spiffs/MELODIES/%s.txt", filename);
    return awtrix_rtttl_play_file(path);
}

bool PeripheryManager::playRTTTL(const char *rtttl) {
    auto &cfg = CONFIG;
    if (!cfg.soundActive) return false;
    if (cfg.dfplayerActive) {
        /* DFPlayer cannot synthesise RTTTL; the original behaviour was to
         * silently ignore RTTTL strings when DFPlayer is the active path
         * (it plays preset MP3s instead). */
        ESP_LOGD(TAG, "playRTTTL ignored: DFPlayer is active");
        return false;
    }
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
    if (CONFIG.dfplayerActive) return awtrix_dfp_is_playing();
    return awtrix_rtttl_is_playing();
}

void PeripheryManager::stopSound() {
    if (CONFIG.dfplayerActive) {
        awtrix_dfp_stop();
        return;
    }
    awtrix_rtttl_stop();
}

void PeripheryManager::setVolume(uint8_t vol_0_30) {
    if (vol_0_30 > 30) vol_0_30 = 30;
    if (CONFIG.dfplayerActive) {
        awtrix_dfp_set_volume(vol_0_30);
    } else {
        awtrix_buzzer_set_volume(vol_0_30);
    }
    AwtrixConfig::Guard guard(CONFIG);
    CONFIG.soundVolume = vol_0_30;
}

void PeripheryManager::r2d2(const char *msg) {
    if (CONFIG.dfplayerActive) {
        /* r2d2 is a short tonal sequence — not representable as a DFPlayer
         * track. Skip silently when DFPlayer is the active sink (matches the
         * original DFPLAYER_ACTIVE early-return in src/PeripheryManager.cpp). */
        return;
    }
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