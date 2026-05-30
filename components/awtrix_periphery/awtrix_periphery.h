#pragma once

#ifdef __cplusplus

#include <functional>

#include "awtrix_hal.h"
#include "awtrix_io.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "freertos/FreeRTOS.h"

/* ── PeripheryManager – replaces the original Arduino class ────
 * Owns: 4 buttons, battery/ldr ADC, I2C sensor, buzzer.
 * Hooks back into DisplayManager / MQTTManager / ServerManager via
 * callback setters.
 */

enum {
    BTN_LEFT   = 0,
    BTN_SELECT = 1,
    BTN_RIGHT  = 2,
    BTN_RESET  = 3,
    BTN_COUNT  = 4,
};

/* Button event enum — kept for external API compatibility */
typedef enum {
    BTN_EVENT_NONE = 0,
    BTN_EVENT_PRESSED,
    BTN_EVENT_DOUBLE_PRESS,
    BTN_EVENT_LONG_PRESS,       /* ≥ 1000 ms */
    BTN_EVENT_VERY_LONG_PRESS,  /* ≥ 5000 ms */
} btn_event_t;

/* callback:  button_index, event_type */
using ButtonCallback = std::function<void(int, btn_event_t)>;

class PeripheryManager {
public:
    static PeripheryManager &get() {
        static PeripheryManager inst;
        return inst;
    }

    void setup();
    void tick();

    /* button callbacks – set by main to hook UI/game/MQTT logic */
    ButtonCallback onButtonEvent;

    /* called from iot_button callback to enqueue an event */
    void enqueueBtnEvent(int idx, btn_event_t evt) {
        if (idx >= 0 && idx < BTN_COUNT) {
            portENTER_CRITICAL(&m_btnLock);
            m_pendingBtnEvent[idx] = evt;
            portEXIT_CRITICAL(&m_btnLock);
        }
    }
    void clearVeryLongFired(int idx) {
        if (idx >= 0 && idx < BTN_COUNT) {
            portENTER_CRITICAL(&m_btnLock);
            m_veryLongFired[idx] = false;
            portEXIT_CRITICAL(&m_btnLock);
        }
    }

    /* audio */
    void playBootSound();
    bool playFromFile(const char *filename);
    bool playRTTTL(const char *rtttl);
    bool parseSound(const char *json);
    bool isPlaying();
    void stopSound();
    void setVolume(uint8_t vol_0_30);
    void r2d2(const char *msg);

    /* sensor */
    void readSensors();

    /* stats */
    uint64_t readUptimeSec();

private:
    PeripheryManager() = default;
    portMUX_TYPE    m_btnLock = portMUX_INITIALIZER_UNLOCKED;
    button_handle_t m_btns[BTN_COUNT];
    btn_event_t     m_pendingBtnEvent[BTN_COUNT] = {BTN_EVENT_NONE};
    bool            m_veryLongFired[BTN_COUNT]    = {false};
    sensor_type_t   m_sensorType = SENSOR_NONE;

    /* median/mean buffers */
    uint16_t m_medBatBuf[7], m_meanBatBuf[7];
    uint16_t m_medLdrBuf[7], m_meanLdrBuf[7];
    median_filter_t m_mfBat, m_mfLdr;
    mean_filter_t   m_afBat, m_afLdr;

    /* timing */
    uint64_t m_lastBatTempHum = 0;
    uint64_t m_lastLdr = 0;
    uint64_t m_bootTime = 0;
};

#endif /* __cplusplus */