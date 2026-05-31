#pragma once

#include "awtrix_hal.h"


#ifdef __cplusplus

#include <string>

#include "awtrix_color.h"   /* CRGB only — keeps awtrix_config from publicly
                              pulling the Matrix / led_matrix / framebuffer
                              tree that lives in awtrix_display. */

/* Pack R: global scroll-speed multiplier ported from src/Globals.cpp:450
 * (`double movementFactor = 0.5`). Used as a constant px/frame baseline by
 * customApp + notification scroll math. Anchor formula (mirrors original):
 *   scrollposition -= movementFactor * (effective_speed / 100.0f)
 * where effective_speed is CONFIG.scroll_speed for the global default or
 * the per-app overrides (CustomApp.scrollSpeed / Notification.scrollSpeed).
 * Held as inline constexpr so it has zero runtime cost and stays in sync
 * across translation units. */
inline constexpr float awtrix_movement_factor = 0.5f;

/* ── Effect / overlay enums ───────────────────────────────────── */
enum BgEffect {
    BG_NONE = -1,
    BG_RAINBOW = 0,
    BG_STARS = 1,
    BG_COUNT
};

enum OverlayEffect : uint8_t {
    OVERLAY_NONE    = 0,
    OVERLAY_TIME    = 1,
    OVERLAY_DRIZZLE = 2,
    OVERLAY_RAIN    = 3,
    OVERLAY_SNOW    = 4,
    OVERLAY_STORM   = 5,
    OVERLAY_THUNDER = 6,
    OVERLAY_FROST   = 7,
    OVERLAY_COUNT
};

/* String-to-enum helper, mirrors the original effects.cpp::getOverlay(). */
OverlayEffect parseOverlayName(const char *name);

/* ════════════════════════════════════════════════════════════════
 *               AwtrixConfig — extending the singleton
 * ════════════════════════════════════════════════════════════════
 *
 * Adding a new persisted setting touches 5 locations (in this exact
 * order — do them top-down so the build always stays green):
 *
 *   1. awtrix_globals.h  — add the field declaration to AwtrixConfig
 *                          (this struct, below this comment).
 *
 *   2. awtrix_globals.cpp::setDefaults()
 *                        — set the default value. Pick something safe
 *                          for first-boot devices.
 *
 *   3. awtrix_globals.cpp::load()
 *                        — read from NVS with SETTINGS_GET_{U8,U16,U32,
 *                          BOOL,STR,FLOAT}.  Use a short, stable key
 *                          (≤8 chars; NVS limit is 15).  If the key
 *                          name ever changes, add a one-shot migration
 *                          like awtrix_settings_load_u32_with_legacy().
 *
 *   4. awtrix_globals.cpp::save()
 *                        — persist with awtrix_settings_save_{u8,u32,str,...}.
 *                          Must use the same NVS key as load().
 *
 *   5. awtrix_globals.cpp::toJson()
 *                        — expose the field in /api/settings and
 *                          /api/stats responses.  Use a JSON key that
 *                          matches what HA / web UI expects (typically
 *                          the same as the NVS key in uppercase).
 *
 * For *runtime-only* fields (currentTemp, batteryRaw, ...) skip steps 3-4
 * but you still need 1-2-5.
 *
 * For settable fields, also add a parser branch in:
 *
 *   6. awtrix_core/DisplayManager.cpp::setNewSettings()
 *                        — let /api/settings + MQTT settings topic write
 *                          the field.  Handle legacy alternative keys here
 *                          (e.g. GAMMA accepts float, GAM accepts int).
 *
 * For Home Assistant integration, also add an entity in:
 *
 *   7. awtrix_network/awtrix_mqtt.cpp::awtrix_mqtt_publish_ha_discovery()
 *                        — if the field should appear in HA's UI.
 *
 * The audit trail (`git log` on awtrix_globals.cpp) has examples of every
 * field type. Steps 1-5 always change together; if you ever see them
 * drift apart in a future PR, that's a sign someone copy-pasted partially.
 *
 * Why no X-macro? Tried in round 8 (E2). Field types are too heterogeneous
 * (10 native types + std::string + CRGB + enum + non-trivial defaults like
 * cJSON-parsed colors), so the macro table would need 7+ different
 * AC_FIELD_xxx variants and still wouldn't cover validation logic in
 * setNewSettings. The macro-driven approach inflates `.def` files,
 * scrambles stack frame names during debugging, and ultimately saves
 * fewer lines than a thin checklist comment.  We chose readability.
 *
 * ──────────────────────────────────────────────────────────────── */

/* ── Globals (replaces original scattered extern variables) ───── */
struct AwtrixConfig {
    /* identity */
    std::string uniqueID;
    std::string hostname;
    const char   *version;

    /* network */
    std::string net_ip, net_gw, net_sn, net_pdns, net_sdns;
    bool        net_static;
    bool        ap_mode;

    /* MQTT */
    std::string mqtt_host, mqtt_user, mqtt_pass, mqtt_prefix;
    uint16_t    mqtt_port;
    bool        ha_discovery;
    std::string ha_prefix;
    bool        io_broker;

    /* misc connectivity */
    std::string auth_user, auth_pass;
    int         web_port;
    uint32_t    ap_timeout;
    bool        debug_mode;

    /* NTP */
    std::string ntp_server, ntp_tz;

    /* display */
    int         brightness;
    bool        auto_brightness;
    uint8_t     min_brightness, max_brightness;
    float       ldr_factor, ldr_gamma;
    bool        ldr_on_ground;

    int         matrix_fps;
    int         matrix_layout;
    bool        mirror_display;
    bool        rotate_screen;
    bool        matrix_off;
    bool        uppercase_letters;
    bool        auto_transition;
    bool        block_navigation;
    bool        swap_buttons;
    int         scroll_speed;

    /* color */
    uint32_t    textColor888;
    uint32_t    timeColor, dateColor, tempColor, humColor, batColor;
    uint32_t    calendarHeader, calendarText, calendarBody;
    uint32_t    wdcActive, wdcInactive;
    uint8_t     gamma; /* stored as int, used as float gamma = x/10 */

    /* formatting */
    std::string timeFormat, dateFormat;
    int         timeMode;
    bool        showSeconds, showWeekday, startOnMonday;

    /* apps visibility */
    bool        showTime, showDate, showTemp, showHum, showBat, showWeather;

    /* transition */
    int         transEffect;
    int         timePerTransition;
    long        timePerApp;

    /* sensors */
    uint8_t     tempSensorType;
    float       tempOffset, humOffset;
    int         tempDecimalPlaces;
    bool        sensorReading;
    bool        isCelsius;

    /* battery */
    uint16_t    minBattery, maxBattery;

    /* sound */
    bool        soundActive;
    uint8_t     soundVolume;
    std::string bootSound;
    bool        buzzerVolume;
    /* When true, all sound output is routed through the DFPlayer Mini MP3
     * module on UART2 (port of original DFPLAYER_ACTIVE). When false (the
     * default), playback falls back to the on-board buzzer + RTTTL parser. */
    bool        dfplayerActive;

    /* effects */
    int         bgEffect;
    OverlayEffect globalOverlay;

    /* stats */
    long        statsInterval;

    /* misc */
    bool        newYear;
    std::string buttonCallback;

    /* runtime (not persisted) */
    float       currentTemp, currentHum, currentLux;
    uint16_t    ldrRaw, batteryRaw;
    uint8_t     batteryPercent;
    std::string currentApp;
    bool        artnetMode, moodlightMode;
    long        receivedMessages;
    bool        updateAvailable;
    bool        sensorsStable;

    CRGB        colorCorrection;
    CRGB        colorTemperature;

    /* singleton */
    static AwtrixConfig &get() {
        static AwtrixConfig c;
        return c;
    }

    /* ── Synchronization ──────────────────────────────────────── */
    void lock() const;
    void unlock() const;

    class Guard {
    public:
        explicit Guard(const AwtrixConfig &cfg) : m_cfg(cfg) { m_cfg.lock(); }
        ~Guard() { m_cfg.unlock(); }
        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;
    private:
        const AwtrixConfig &m_cfg;
    };

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

    /* A3: opaque mutex pointer. Was `SemaphoreHandle_t m_lock` until r7;
     * the FreeRTOS handle type pulled `freertos/semphr.h` into every TU
     * that included this header. The .cpp implementation casts back to
     * `SemaphoreHandle_t` on every entry. Same PImpl-lite pattern that
     * hides iot_button's button_handle_t in awtrix_periphery.h. */
    mutable void *m_lock = nullptr;
};

/* convenience alias */
#define CONFIG (AwtrixConfig::get())

#endif /* __cplusplus */
