#pragma once

#ifdef __cplusplus

#include <functional>

#include "awtrix_hal.h"
#include "awtrix_io.h"           /* sensor_type_t, median_filter_t, mean_filter_t */
#include "freertos/FreeRTOS.h"   /* portMUX_TYPE — embedded as m_btnLock member */

/* ── PeripheryManager – replaces the original Arduino class ────
 * Owns: 4 buttons, battery/ldr ADC, I2C sensor, buzzer.
 * Hooks back into DisplayManager / MQTTManager / ServerManager via
 * callback setters.
 *
 * Header hygiene (round 3): iot_button.h / button_gpio.h are deliberately
 * NOT included here. m_btns is declared as an opaque void* array so callers
 * never need to see the iot_button SDK types just to use PeripheryManager.
 * That lets `button` stay in the periphery's PRIV_REQUIRES instead of being
 * forced into the public REQUIRES set. */

enum
{
    BTN_LEFT = 0,
    BTN_SELECT = 1,
    BTN_RIGHT = 2,
    BTN_RESET = 3,
    BTN_COUNT = 4,
};

/* Button event enum — kept for external API compatibility */
typedef enum
{
    BTN_EVENT_NONE = 0,
    BTN_EVENT_PRESSED,
    BTN_EVENT_DOUBLE_PRESS,
    BTN_EVENT_LONG_PRESS, /* ≥ 1000 ms */
    BTN_EVENT_VERY_LONG_PRESS, /* ≥ 5000 ms */
} btn_event_t;

/* callback:  button_index, event_type */
using ButtonCallback = std::function<void(int, btn_event_t)>;

class PeripheryManager
{
public:
    static PeripheryManager& get()
    {
        static PeripheryManager inst;
        return inst;
    }

    void setup();
    void tick();

    /* button callbacks – set by main to hook UI/MQTT logic */
    ButtonCallback onButtonEvent;

    /* Called from the iot_button SDK callback (inside the .cpp). Out-of-line
     * so the implementation can use portENTER_CRITICAL without forcing every
     * header consumer to pull FreeRTOS types they don't otherwise need. */
    void enqueueBtnEvent(int idx, btn_event_t evt);
    void clearVeryLongFired(int idx);

    /* audio */
    void playBootSound();
    bool playFromFile(const char* filename);
    bool playRTTTL(const char* rtttl);
    bool parseSound(const char* json);
    bool isPlaying();
    void stopSound();
    void setVolume(uint8_t vol_0_30);
    void r2d2(const char* msg);

    /* sensor */
    void readSensors();

    /* stats */
    uint64_t readUptimeSec();

private:
    PeripheryManager() = default;
    portMUX_TYPE m_btnLock = portMUX_INITIALIZER_UNLOCKED;
    /* Opaque iot_button handles. Declared as void* so external compilation
     * units never need to see `button_handle_t` (a typedef'd struct pointer
     * from the espressif__button managed component). The .cpp uses
     * reinterpret_cast<button_handle_t> on every access. */
    void* m_btns[BTN_COUNT] = {nullptr, nullptr, nullptr, nullptr};
    btn_event_t m_pendingBtnEvent[BTN_COUNT] = {BTN_EVENT_NONE};
    bool m_veryLongFired[BTN_COUNT] = {false};
    sensor_type_t m_sensorType = SENSOR_NONE;

    /* median/mean buffers */
    uint16_t m_medBatBuf[7], m_meanBatBuf[7];
    uint16_t m_medLdrBuf[7], m_meanLdrBuf[7];
    median_filter_t m_mfBat, m_mfLdr;
    mean_filter_t m_afBat, m_afLdr;

    /* timing */
    uint64_t m_lastBatTempHum = 0;
    uint64_t m_lastLdr = 0;
    uint64_t m_bootTime = 0;
};

#endif /* __cplusplus */
