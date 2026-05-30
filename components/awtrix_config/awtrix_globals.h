#pragma once

#include "awtrix_hal.h"


#ifdef __cplusplus

#include <string>

#include "matrix_cpp.h"

/* ── Effect / overlay enums ───────────────────────────────────── */
enum BgEffect
{
    BG_NONE = -1,
    BG_RAINBOW = 0,
    BG_STARS = 1,
    BG_COUNT
};

enum OverlayEffect : uint8_t
{
    OVERLAY_NONE = 0,
    OVERLAY_TIME = 1,
    OVERLAY_DRIZZLE = 2,
    OVERLAY_RAIN = 3,
    OVERLAY_SNOW = 4,
    OVERLAY_STORM = 5,
    OVERLAY_THUNDER = 6,
    OVERLAY_FROST = 7,
    OVERLAY_COUNT
};

/* String-to-enum helper, mirrors the original effects.cpp::getOverlay(). */
OverlayEffect parseOverlayName(const char* name);

/* ── Globals (replaces original scattered extern variables) ───── */
struct AwtrixConfig
{
    /* identity */
    std::string uniqueID;
    std::string hostname;
    const char* version;

    /* network */
    std::string net_ip, net_gw, net_sn, net_pdns, net_sdns;
    bool net_static;
    bool ap_mode;

    /* MQTT */
    std::string mqtt_host, mqtt_user, mqtt_pass, mqtt_prefix;
    uint16_t mqtt_port;
    bool ha_discovery;
    std::string ha_prefix;
    bool io_broker;

    /* misc connectivity */
    std::string auth_user, auth_pass;
    int web_port;
    uint32_t ap_timeout;
    bool debug_mode;

    /* NTP */
    std::string ntp_server, ntp_tz;

    /* display */
    int brightness;
    bool auto_brightness;
    uint8_t min_brightness, max_brightness;
    float ldr_factor, ldr_gamma;
    bool ldr_on_ground;

    int matrix_fps;
    int matrix_layout;
    bool mirror_display;
    bool rotate_screen;
    bool matrix_off;
    bool uppercase_letters;
    bool auto_transition;
    bool block_navigation;
    bool swap_buttons;
    int scroll_speed;

    /* color */
    uint32_t textColor888;
    uint32_t timeColor, dateColor, tempColor, humColor, batColor;
    uint32_t calendarHeader, calendarText, calendarBody;
    uint32_t wdcActive, wdcInactive;
    uint8_t gamma; /* stored as int, used as float gamma = x/10 */

    /* formatting */
    std::string timeFormat, dateFormat;
    int timeMode;
    bool showSeconds, showWeekday, startOnMonday;

    /* apps visibility */
    bool showTime, showDate, showTemp, showHum, showBat, showWeather;

    /* transition */
    int transEffect;
    int timePerTransition;
    long timePerApp;

    /* sensors */
    uint8_t tempSensorType;
    float tempOffset, humOffset;
    int tempDecimalPlaces;
    bool sensorReading;
    bool isCelsius;

    /* battery */
    uint16_t minBattery, maxBattery;

    /* sound */
    bool soundActive;
    uint8_t soundVolume;
    std::string bootSound;
    bool buzzerVolume;

    /* effects */
    int bgEffect;
    OverlayEffect globalOverlay;

    /* stats */
    long statsInterval;

    /* misc */
    bool newYear;
    std::string buttonCallback;

    /* runtime (not persisted) */
    float currentTemp, currentHum, currentLux;
    uint16_t ldrRaw, batteryRaw;
    uint8_t batteryPercent;
    std::string currentApp;
    bool artnetMode, moodlightMode;
    long receivedMessages;
    bool updateAvailable;
    bool sensorsStable;
    bool gameActive;

    CRGB colorCorrection;
    CRGB colorTemperature;

    /* singleton */
    static AwtrixConfig& get()
    {
        static AwtrixConfig c;
        return c;
    }

    /* ── Persistence ──────────────────────────────────────────── */
    void load();
    void save();
    void eraseAll();

    /* ── JSON helpers ─────────────────────────────────────────── */
    std::string toJson() const;

private:
    AwtrixConfig();
    void loadDevSettings();
    void setDefaults();
};

/* convenience alias */
#define CONFIG (AwtrixConfig::get())

#endif /* __cplusplus */
