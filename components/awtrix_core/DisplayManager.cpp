#include "DisplayManager.h"
#include "awtrix_globals.h"
#include "effects_core.h"
#include "awtrix_events.h"     /* EVENTS.rtttl(...) — replaces direct awtrix_io.h dep */
#include "awtrix_command_bus.h"
#include "awtrix_menumanager.h"
#include "awtrix_display_snapshot.h"
#include "awtrix_render.h"
#include "awtrix_overlay_registry.h"
#include "awtrix_icon_loader.h"   /* Pack K+L: SPIFFS 8x8 icon loader (.rgb565/.jpg) */
#include "awtrix_placeholders.h"  /* P1-D: MQTT topic placeholder registry */
#include "awtrix_color_utils.h"   /* A1: shared HSV/lerp/fx/case/scale helpers */
#include "awtrix_system_state.h"  /* L1: SYS_TRANSITION at moodlight/power/sleep */
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cmath>
#include <algorithm>
#include <time.h>
#include <dirent.h>           /* loadCustomApps() walks /spiffs/CUSTOMAPPS */
#include <strings.h>          /* strcasecmp */
#include <sys/stat.h>
#include <unistd.h>           /* unlink() in parseCustomPage */

#define TAG "disp"

extern "C" bool awtrix_artnet_take_frame(uint32_t* rgb888, int max_pixels, int* out_pixels) __attribute__
((weak));
extern "C" void awtrix_power_sleep_parser(const char* json);
/* P1-D: weak link to the MQTT subscriber so parseCustomPage can self-register
 * {{topic}} placeholders without dragging awtrix_network into awtrix_core's
 * REQUIRES (network already depends on core; closing the loop would create a
 * cycle). When MQTT is disabled the symbol resolves to nullptr and the
 * placeholder still works for {{CONFIG.*}} static fallbacks. */
extern "C" void awtrix_mqtt_subscribe(const char* topic_suffix) __attribute__
((weak));

static uint64_t _now() { return esp_timer_get_time() / 1000; }

static void update_display_snapshot_from_matrix(Matrix* matrix)
{
    if (!matrix) return;
    const int total = matrix->width() * matrix->height();
    if (total <= 0) return;
    uint32_t pixels[MATRIX_WIDTH * MATRIX_HEIGHT];
    const int maxPixels = MATRIX_WIDTH * MATRIX_HEIGHT;
    const int count = total > maxPixels ? maxPixels : total;
    const CRGB* leds = matrix->getLeds();
    if (!leds) return;
    for (int i = 0; i < count; ++i)
    {
        pixels[i] = ((uint32_t)leds[i].r << 16) | ((uint32_t)leds[i].g << 8) | (uint32_t)leds[i].b;
    }
    DISPLAY_SNAPSHOT.updateFrameRgb888(pixels, count, matrix->width(), matrix->height());
}

/* ── Forward declarations / file-scope shared state ─────────────
 * These need to be visible to tick() / updateAppVector() which appear
 * before their definitions later in the file. */
/* B2: implementation lives in DisplayManager_customApp.cpp so the
 * customApp rendering pipeline can be edited without recompiling the
 * 2300-line main DisplayManager translation unit. The forward decl
 * matches the signature used by APP_REGISTRY for each custom slot. */
void renderCustomApp(Matrix& m, UiState& state, int16_t x, int16_t y, GifPlayer*);
static uint32_t s_moodlightColor = 0x000000;
static bool s_moodlightActive = false;

/* ── Built-in app callbacks ───────────────────────────────────
 * Pack A — full re-port of src/Apps.cpp (~300 lines of original) into
 * compact equivalents that honour every relevant CONFIG.* field. The five
 * apps below mirror the original feature set 1:1 within the limits of
 * what the ESP-IDF Matrix API can render (BigTime GIF backdrop is the only
 * mode that stays stubbed because the GIF decoder lives in pack K). */

/* Helper: draw the bottom 7-segment weekday strip used by TimeApp /
 * DateApp (mode 0 with SHOW_WEEKDAY and mode 2). startOnMonday rotates the
 * leftmost segment from Sunday→Monday; today's segment uses wdcActive,
 * the rest use wdcInactive. */
static void awtrix_draw_weekday_strip(Matrix& m, int16_t x, int16_t y,
                                      const struct tm* ti)
{
    const auto& c = CONFIG;
    /* tm_wday: 0 = Sunday, 1..6 = Mon..Sat. Original used "today index" in
     * [0..6] aligned to startOnMonday. */
    int todayIdx = c.startOnMonday
                       ? ((ti->tm_wday + 6) % 7) /* Mon..Sun */
                       : ti->tm_wday; /* Sun..Sat */
    const int wdPosY = y + 7; /* bottom row of 8-px tall slot */
    for (int i = 0; i < 7; i++)
    {
        int lineStart = i * 4 + 2; /* 4 px wide + 1 px gap */
        int lineEnd = lineStart + 3;
        uint32_t col = (i == todayIdx) ? c.wdcActive : c.wdcInactive;
        m.drawLine(x + lineStart, wdPosY, x + lineEnd, wdPosY, col);
    }
}

/* Helper: 4-row binary clock used by TimeApp mode 4 (Binary).
 * Lays out HH (2 columns) + MM (2 columns) as 4-bit-tall stacks at left
 * of the matrix. Pixels filled with `col` when the bit is set. */
static void awtrix_draw_binary_clock(Matrix& m, int16_t x, int16_t y,
                                     int hour, int minute, uint32_t col)
{
    int digits[4] = {hour / 10, hour % 10, minute / 10, minute % 10};
    for (int d = 0; d < 4; d++)
    {
        for (int b = 0; b < 4; b++)
        {
            if (digits[d] & (1 << b))
            {
                int px = x + 1 + d * 3;
                int py = y + 6 - b * 2; /* bit 0 on bottom row */
                m.drawPixel(px, py, col);
                m.drawPixel(px + 1, py, col);
            }
        }
    }
}

void TimeApp(Matrix& m, UiState&, int16_t x, int16_t y, GifPlayer*)
{
    const auto& c = CONFIG;
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    if (!ti) return;
    uint32_t col = c.timeColor ? c.timeColor : c.textColor888;
    char buf[32];

    switch (c.timeMode)
    {
    case 1:
        {
            /* Calendar box: red header w/ weekday number, white body w/ date */
            m.fillRect(x + 11, y, 21, 8, c.calendarBody);
            m.fillRect(x + 11, y, 21, 3, c.calendarHeader);
            /* weekday short name (3-letter), e.g. "MON" */
            char wd[4] = "   ";
            const char* weekdays = "SunMonTueWedThuFriSat";
            memcpy(wd, weekdays + (ti->tm_wday * 3), 3);
            m.setTextColor(c.calendarText);
            m.setCursor(x + 13, y + 2);
            m.print(wd);
            snprintf(buf, sizeof(buf), "%02d", ti->tm_mday);
            m.setCursor(x + 18, y + 7);
            m.print(buf);
            /* Fall through to also draw the time on the left half. */
            m.setTextColor(col);
            snprintf(buf, sizeof(buf), "%2d:%02d", ti->tm_hour, ti->tm_min);
            m.setCursor(x, y + 1);
            m.print(buf);
            return;
        }
    case 4:
        {
            /* Binary clock */
            awtrix_draw_binary_clock(m, x, y, ti->tm_hour, ti->tm_min, col);
            return;
        }
    case 3:
        {
            /* H:M:S */
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                     ti->tm_hour, ti->tm_min, ti->tm_sec);
            m.setCursor(x + 2, y + 1);
            m.setTextColor(col);
            m.print(buf);
            return;
        }
    case 5:
        {
            /* BigTime — GIF backdrop unavailable until pack K; fallback to mode 0 */
            /* fall-through */
        }
    case 0:
    case 2:
    default:
        {
            /* Default time render: use timeFormat (strftime), with blinking ':'
         * separator when showSeconds is on. */
            const char* fmt = c.timeFormat.empty() ? "%H:%M" : c.timeFormat.c_str();
            if (c.showSeconds && (ti->tm_sec & 1))
            {
                /* On odd seconds replace ':' with ' ' for a 1Hz blink, matching
             * the original behaviour. Work on a copy of buf. */
                strftime(buf, sizeof(buf), fmt, ti);
                for (char* p = buf; *p; p++) if (*p == ':') *p = ' ';
            }
            else
            {
                strftime(buf, sizeof(buf), fmt, ti);
            }
            m.setCursor(x + 2, y + 1);
            m.setTextColor(col);
            m.print(buf);
            if ((c.timeMode == 2) || (c.timeMode == 0 && c.showWeekday))
            {
                awtrix_draw_weekday_strip(m, x, y, ti);
            }
            return;
        }
    }
}

void DateApp(Matrix& m, UiState&, int16_t x, int16_t y, GifPlayer*)
{
    const auto& c = CONFIG;
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    if (!ti) return;
    uint32_t col = c.dateColor ? c.dateColor : c.textColor888;
    char buf[32];
    const char* fmt = c.dateFormat.empty() ? "%m.%d" : c.dateFormat.c_str();
    strftime(buf, sizeof(buf), fmt, ti);
    m.setCursor(x + 2, y + 1);
    m.setTextColor(col);
    m.print(buf);
    if (c.showWeekday)
    {
        awtrix_draw_weekday_strip(m, x, y, ti);
    }
}

void TempApp(Matrix& m, UiState&, int16_t x, int16_t y, GifPlayer*)
{
    const auto& c = CONFIG;
    char buf[16];
    float temp = c.currentTemp + c.tempOffset;
    /* °C ↔ °F unit conversion mirrors original IS_CELSIUS branch. */
    if (!c.isCelsius)
    {
        temp = temp * 9.0f / 5.0f + 32.0f;
        snprintf(buf, sizeof(buf), "%.*f\xb0" "F", c.tempDecimalPlaces, temp);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%.*f\xb0" "C", c.tempDecimalPlaces, temp);
    }
    uint32_t col = c.tempColor ? c.tempColor : c.textColor888;
    /* Tiny thermometer icon at left edge: 2-px bulb + 4-px stem. */
    m.drawPixel(x + 1, y + 2, col);
    m.drawPixel(x + 1, y + 3, col);
    m.drawPixel(x + 1, y + 4, col);
    m.drawPixel(x + 2, y + 5, col);
    m.drawPixel(x + 1, y + 5, col);
    m.drawPixel(x + 0, y + 5, col);
    m.setCursor(x + 5, y + 1);
    m.setTextColor(col);
    m.print(buf);
}

void HumApp(Matrix& m, UiState&, int16_t x, int16_t y, GifPlayer*)
{
    const auto& c = CONFIG;
    char buf[12];
    snprintf(buf, sizeof(buf), "%.0f%%", c.currentHum);
    uint32_t col = c.humColor ? c.humColor : c.textColor888;
    /* Small water drop icon. */
    m.drawPixel(x + 1, y + 2, col);
    m.drawPixel(x + 0, y + 3, col);
    m.drawPixel(x + 2, y + 3, col);
    m.drawPixel(x + 0, y + 4, col);
    m.drawPixel(x + 2, y + 4, col);
    m.drawPixel(x + 1, y + 5, col);
    m.setCursor(x + 5, y + 1);
    m.setTextColor(col);
    m.print(buf);
}

void BatApp(Matrix& m, UiState&, int16_t x, int16_t y, GifPlayer*)
{
    const auto& c = CONFIG;
    char buf[12];
    int pct = c.batteryPercent;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    /* P0-B: charging detection. The Ulanzi has no dedicated USB-presence
     * pin, so we infer charging from the ADC voltage saturating near the
     * maxBattery calibration value (the original firmware did the same
     * thing). 97% of maxBattery gives enough ADC headroom that intermittent
     * noise on a freshly-charged battery doesn't false-trigger. */
    const bool charging = c.batteryRaw > 0 &&
        c.maxBattery > 0 &&
        c.batteryRaw >= (uint16_t)((uint32_t)c.maxBattery * 97 / 100);

    if (charging)
    {
        snprintf(buf, sizeof(buf), "+%d%%", pct);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%d%%", pct);
    }

    uint32_t col = c.batColor ? c.batColor : c.textColor888;
    /* 8x4 battery outline at left edge: thin nub on the right, hollow body.
     * Body x=0..6, y=2..5; nub x=7, y=3..4. Fill width = pct/100 * 5 px. */
    m.drawRect(x + 0, y + 2, 7, 4, col);
    m.drawPixel(x + 7, y + 3, col);
    m.drawPixel(x + 7, y + 4, col);
    int filled = (pct * 5) / 100;
    if (filled > 0)
    {
        /* Fill color: red below 20%, yellow 20..50, green >50 (matches the
         * original PeripheryManager battery-fill palette). */
        uint32_t fillColor = (pct < 20)
                                 ? AwtrixColors::kBatteryLow
                                 : (pct < 50)
                                 ? AwtrixColors::kBatteryMid
                                 : AwtrixColors::kBatteryHigh;
        m.fillRect(x + 1, y + 3, filled, 2, fillColor);
    }
    /* P0-B: charging bolt above the battery — 3-pixel lightning shape that
     * blinks at 1 Hz so it reads as activity rather than another decoration. */
    if (charging)
    {
        const long nowMs = (long)(esp_timer_get_time() / 1000);
        if ((nowMs / 500) & 1)
        {
            m.drawPixel(x + 3, y + 0, AwtrixColors::kChargingBolt);
            m.drawPixel(x + 2, y + 1, AwtrixColors::kChargingBolt);
            m.drawPixel(x + 4, y + 1, AwtrixColors::kChargingBolt);
        }
    }
    m.setCursor(x + 10, y + 1);
    m.setTextColor(col);
    m.print(buf);
}

/* Static storage no longer required: MatrixDisplayUi::setOverlays takes a
 * const std::vector<OverlayCallback>& and copies the entries internally. */

/* ── Setup ───────────────────────────────────────────────────── */
void DisplayManager::setup(Matrix* m)
{
    m_matrix = m;
    m_ui = new MatrixDisplayUi(m);
    m_ui->init();
    m_ui->setOverlays(awtrix_default_overlays());
    applyAllSettings();
}

void DisplayManager::applyAllSettings()
{
    if (!m_ui) return;
    auto& c = CONFIG;
    m_ui->setTargetFPS(c.matrix_fps);
    m_ui->setTimePerApp(c.timePerApp);
    m_ui->setTimePerTransition(c.timePerTransition);
    if (!c.auto_brightness)
    {
        if (m_matrix) m_matrix->setBrightness(c.brightness);
    }
    /* Apply rotation live: when the user flips SB / ROT we want the next
     * frame to come out the right way up without a reboot. setRotation(2)
     * is 180° (Ulanzi clock); 0 is the default. */
    if (m_matrix) m_matrix->setRotation(c.rotate_screen ? 2 : 0);
    /* P0-A: push the FastLED-style color correction + temperature into the
     * Matrix HAL so /api/settings { CCORRECTION, CTEMP } actually changes
     * the displayed colors. Without this the values were only persisted to
     * NVS but the matrix kept the boot-time defaults forever. */
    if (m_matrix)
    {
        m_matrix->setCorrection(c.colorCorrection);
        m_matrix->setTemperature(c.colorTemperature);
    }
    /* Apply gamma if configured (acts as a clamp; per-frame gamma8 still
     * runs in tick()). Original tracked a float gamma; we store uint8_t. */
    if (m_matrix&& c
    .
    gamma > 0
    )
    m_matrix->applyGamma((float)c.gamma / 10.0f);
    m_ui->setBackgroundEffect(c.bgEffect);
    m_ui->enableAutoTransition();
    if (!c.auto_transition) m_ui->disableAutoTransition();
}

static uint8_t command_source_to_notification_source(uint8_t source)
{
    switch (source)
    {
    case AWTRIX_COMMAND_SOURCE_HTTP: return 1;
    case AWTRIX_COMMAND_SOURCE_MQTT: return 2;
    case AWTRIX_COMMAND_SOURCE_BUTTON: return 3;
    default: return 0;
    }
}

void DisplayManager::processCommand(const AwtrixCommand& command)
{
    switch (command.type)
    {
    case AwtrixCommandType::Notify:
        generateNotification(command_source_to_notification_source(command.source), command.payload.c_str());
        break;
    case AwtrixCommandType::DismissNotify:
        dismissNotify();
        break;
    case AwtrixCommandType::CustomApp:
        parseCustomPage(command.name.c_str(), command.payload.c_str(), false);
        break;
    case AwtrixCommandType::Settings:
        setNewSettings(command.payload.c_str());
        break;
    case AwtrixCommandType::UpdateApps:
        updateAppVector(command.payload.c_str());
        break;
    case AwtrixCommandType::SwitchApp:
        switchToApp(command.payload.c_str());
        break;
    case AwtrixCommandType::NextApp:
        nextApp();
        break;
    case AwtrixCommandType::PreviousApp:
        previousApp();
        break;
    case AwtrixCommandType::Power:
        powerStateParse(command.payload.c_str());
        break;
    case AwtrixCommandType::Sleep:
        awtrix_power_sleep_parser(command.payload.c_str());
        break;
    case AwtrixCommandType::Moodlight:
        moodlight(command.payload.c_str());
        break;
    case AwtrixCommandType::ReorderApps:
        reorderApps(command.payload.c_str());
        break;
    case AwtrixCommandType::Indicator:
        indicatorParser(command.index, command.payload.c_str());
        break;
    case AwtrixCommandType::Button:
        if (awtrix_menu_active())
        {
            switch (command.index)
            {
            case 0: awtrix_menu_left();
                break;
            case 1: awtrix_menu_select_short();
                break;
            case 2: awtrix_menu_right();
                break;
            case 3: awtrix_menu_select_long();
                break;
            default: break;
            }
        }
        else
        {
            switch (command.index)
            {
            case 0: leftButton();
                break;
            case 1: selectButton();
                break;
            case 2: rightButton();
                break;
            case 3: awtrix_menu_select_long();
                break;
            default: break;
            }
        }
        break;
    case AwtrixCommandType::SetBrightness:
        {
            /* P1-7: validated brightness parse. atoi() accepted any garbage
             * payload silently (turning the screen off on bad input was a
             * user-visible bug). strtol + range check rejects non-numbers
             * and out-of-range values with a log line so the operator can
             * see what was sent. */
            int bri = -1;
            if (command.payload.empty())
            {
                /* `index` is the in-band uint8 fast path used by buttons /
                 * periphery auto-brightness. Always valid by construction. */
                bri = (int)command.index;
            }
            else
            {
                const char* s = command.payload.c_str();
                char* endptr = nullptr;
                long parsed = strtol(s, &endptr, 10);
                if (endptr && endptr != s && *endptr == '\0'
                    && parsed >= 0 && parsed <= 255)
                {
                    bri = (int)parsed;
                }
                else
                {
                    ESP_LOGW(TAG, "SetBrightness rejected: bad payload \"%s\" (src=%u)",
                             s, (unsigned)command.source);
                    /* Drop the command rather than fall back to 0 — going
                     * dark silently is worse than ignoring an injection. */
                    break;
                }
            }
            setBrightness(bri);
            break;
        }
    case AwtrixCommandType::ShowSleepScreen:
        showSleepAnimation();
        break;
    default:
        ESP_LOGW(TAG, "Unhandled display command type=%u", (unsigned)command.type);
        break;
    }
}

void DisplayManager::processPendingEvents(int maxEvents)
{
    if (maxEvents <= 0) return;
    for (int i = 0; i < maxEvents; ++i)
    {
        AwtrixCommand command;
        if (!awtrix_command_bus_receive(command, 0)) break;
        processCommand(command);
    }
}

/* ── Tick ────────────────────────────────────────────────────── */
void DisplayManager::tick()
{
    processPendingEvents();
    if (!m_ui) return;
    if (m_matrixOff)
    {
        RENDER_ENGINE.setMode(AwtrixDisplayMode::Off);
        return;
    }

    /* Pack J: AP_MODE indicator — when the device is in soft-AP fallback
     * we paint a centered "AP MODE" string instead of running app rotation.
     * Mirrors the original src/DisplayManager.cpp::tick() AP_MODE branch. */
    if (CONFIG.ap_mode && m_matrix)
    {
        RENDER_ENGINE.setMode(AwtrixDisplayMode::Boot); /* reuse Boot mode for "splash"-like UI */
        m_matrix->clear();
        HSVtext(0, 6, "AP MODE", true, 1); /* centered, uppercase */
        m_matrix->show();
        update_display_snapshot_from_matrix(m_matrix);
        return;
    }

    if (CONFIG.artnetMode && m_matrix && &awtrix_artnet_take_frame)
    {
        uint32_t frame[MATRIX_WIDTH * MATRIX_HEIGHT];
        int pixels = 0;
        if (awtrix_artnet_take_frame(frame, MATRIX_WIDTH * MATRIX_HEIGHT, &pixels))
        {
            RENDER_ENGINE.setMode(AwtrixDisplayMode::ArtNet);
            const int width = m_matrix->width();
            const int height = m_matrix->height();
            const int total = width * height;
            if (pixels > total) pixels = total;
            for (int i = 0; i < pixels; ++i)
            {
                const int x = i % width;
                const int y = i / width;
                if (y >= height) break;
                m_matrix->drawPixel(x, y, frame[i]);
            }
            m_matrix->show();
            update_display_snapshot_from_matrix(m_matrix);
            return;
        }
    }

    /* Moodlight short-circuits regular app rendering: every frame we just
     * paint the configured solid color across the whole matrix and push it.
     * Mirrors the original MOODLIGHT_MODE behaviour. Snapshot both vars
     * inside the lock so we never observe a torn (Active=true, Color=0) pair
     * if /api/moodlight is being processed concurrently. */
    bool moodActive;
    uint32_t moodColor;
    {
        Lock _l(&m_dataLock);
        moodActive = s_moodlightActive;
        moodColor = s_moodlightColor;
    }
    if (moodActive && m_matrix)
    {
        RENDER_ENGINE.setMode(AwtrixDisplayMode::Moodlight);
        m_matrix->fillRect(0, 0, m_matrix->width(), m_matrix->height(), moodColor);
        if (CONFIG.gamma > 0) gammaCorrection();
        m_matrix->show();
        update_display_snapshot_from_matrix(m_matrix);
        return;
    }

    RENDER_ENGINE.setMode(NOTIFICATIONS.empty() ? AwtrixDisplayMode::Normal : AwtrixDisplayMode::Notification);
    m_ui->update();

    /* Apply gamma correction to the framebuffer (when configured) before the
     * next show(), mirroring the original FastLED gamma8 pass. */
    if (CONFIG.gamma > 0) gammaCorrection();
    update_display_snapshot_from_matrix(m_ui->getMatrix());

    /* Pack J: customApp lifecycle hooks at the FIXED ↔ IN_TRANSITION edge.
     * When we just entered IN_TRANSITION (frame N-1 = FIXED, frame N = IN_TRANSITION),
     * the original would: (1) prune the customApp that is about to be left if its
     * lifetime expired; (2) reset scroll/icon state on all non-current customApps. */
    if (m_ui->getUiState()->appState == UiState::IN_TRANSITION && !m_appIsSwitching)
    {
        m_appIsSwitching = true;
        checkLifetime(m_ui->getNextAppNumber());
        resetCustomApps();
    }
    else if (m_ui->getUiState()->appState == UiState::FIXED && m_appIsSwitching)
    {
        m_appIsSwitching = false;
    }
}

/* ── App management ──────────────────────────────────────────── */
/* Pack J: customApp lifetime expiry check (called when transitioning into
 * a new app slot). When the next slot is a customApp whose lifetime has
 * elapsed since lastUpdate, original behaviour was:
 *   lifetimeMode == 0  → remove the app outright (from both maps + UI)
 *   lifetimeMode == 1  → flag lifeTimeEnd so the renderer draws a red border
 *                        (the app continues to exist; user must POST again
 *                        to refresh it). */
void DisplayManager::checkLifetime(uint8_t pos)
{
    Lock _l(&m_dataLock);
    const auto& apps = APP_REGISTRY.apps();
    if (pos >= apps.size()) return;
    const std::string& name = apps[pos].first;
    auto it = APP_REGISTRY.customApps().find(name);
    if (it == APP_REGISTRY.customApps().end()) return;

    CustomApp& app = it->second;
    if (app.lifetime == 0) return;

    long now = (long)(esp_timer_get_time() / 1000);
    long age = (now - app.lastUpdate) / 1000;
    if (age < (long)app.lifetime) return;

    if (app.lifetimeMode == 0)
    {
        ESP_LOGI(TAG, "customApp '%s' expired (lifetime=%llu s) — removing",
                 name.c_str(), (unsigned long long)app.lifetime);
        APP_REGISTRY.eraseCustomApp(name);
        APP_REGISTRY.eraseApp(name);
        if (m_ui) m_ui->setApps(APP_REGISTRY.apps());
    }
    else
    {
        app.lifeTimeEnd = true;
    }
}

/* Pack J: reset scroll/icon state on all non-current customApps after a
 * transition, so a re-entry starts fresh (text scroll, icon push state and
 * repeat counter all back to initial). */
void DisplayManager::resetCustomApps()
{
    Lock _l(&m_dataLock);
    auto& customs = APP_REGISTRY.customApps();
    if (customs.empty()) return;

    for (auto& kv : customs)
    {
        CustomApp& app = kv.second;
        if (app.name == m_currentCustomApp) continue;
        /* original (Arduino) start position: scrollposition = (icon?9:0) + textOffset */
        const float baseScroll = (app.iconName.empty() && app.compiledDraw.empty() ? 0.0f : 9.0f);
        app.scrollposition = baseScroll + (float)app.textOffset;
        app.iconPosition = 0;
        app.scrollDelay = 0;
        app.currentRepeat = 0;
        app.iconWasPushed = false;
        app.lifeTimeEnd = false;
    }
}

/* Internal, lock-free implementation. Caller MUST hold m_dataLock. */
static void loadNativeApps_impl(MatrixDisplayUi* ui)
{
    auto& c = CONFIG;
    std::vector<std::pair<std::string, AppCallback>> apps;

    if (c.showTime) apps.push_back({"Time", TimeApp});
    if (c.showDate) apps.push_back({"Date", DateApp});
    if (c.showTemp) apps.push_back({"Temp", TempApp});
    if (c.showHum) apps.push_back({"Hum", HumApp});
    if (c.showBat) apps.push_back({"Bat", BatApp});
    if (c.showWeather) apps.push_back({"Weather", WeatherApp});

    APP_REGISTRY.replaceApps(std::move(apps));
    if (ui) ui->setApps(APP_REGISTRY.apps());
}

void DisplayManager::loadNativeApps()
{
    Lock _l(&m_dataLock);
    loadNativeApps_impl(m_ui);
    setAutoTransition(CONFIG.auto_transition);
}

void DisplayManager::loadCustomApps()
{
    /* Mirrors src/DisplayManager.cpp::loadCustomApps() in the original.
     * Walks /spiffs/CUSTOMAPPS/<name>.json and feeds each through parseCustomPage
     * with preventSave=true so we don't write the same file right back. */
    DIR* dir = opendir("/spiffs/CUSTOMAPPS");
    if (!dir)
    {
        ESP_LOGI(TAG, "No /spiffs/CUSTOMAPPS directory; skip custom apps");
        return;
    }
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr)
    {
        if (de->d_type == DT_DIR) continue;
        const char* fn = de->d_name;
        const char* dot = strrchr(fn, '.');
        if (!dot || strcasecmp(dot, ".json") != 0) continue;

        char path[320];
        snprintf(path, sizeof(path), "/spiffs/CUSTOMAPPS/%s", fn);
        FILE* fp = fopen(path, "r");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz <= 0 || sz > 16 * 1024)
        {
            fclose(fp);
            continue;
        }
        char* buf = (char*)malloc(sz + 1);
        if (!buf)
        {
            fclose(fp);
            continue;
        }
        size_t n = fread(buf, 1, sz, fp);
        fclose(fp);
        buf[n] = '\0';

        /* basename = filename without ".json" */
        std::string name(fn, (size_t)(dot - fn));
        parseCustomPage(name.c_str(), buf, /*preventSave=*/true);
        free(buf);
    }
    closedir(dir);
}

void DisplayManager::nextApp() { if (m_ui) m_ui->nextApp(); }
void DisplayManager::previousApp() { if (m_ui) m_ui->previousApp(); }
void DisplayManager::leftButton() { if (m_ui) m_ui->previousApp(); }
void DisplayManager::rightButton() { if (m_ui) m_ui->nextApp(); }

void DisplayManager::forceNextApp()
{
    if (!m_ui) return;
    m_ui->switchToApp(m_ui->getUiState()->currentApp);
    setAppTime(CONFIG.timePerApp);
}

void DisplayManager::setAppTime(long ms)
{
    if (m_ui) m_ui->setTimePerApp(ms);
}

bool DisplayManager::switchToApp(const char* json)
{
    if (!m_ui) return false;
    cJSON* doc = cJSON_Parse(json);
    if (!doc) return false;
    cJSON* name = cJSON_GetObjectItem(doc, "name");
    if (!name || !name->valuestring)
    {
        cJSON_Delete(doc);
        return false;
    }
    std::string n(name->valuestring);
    cJSON_Delete(doc);

    Lock _l(&m_dataLock);
    int index = APP_REGISTRY.findAppIndex(n);
    if (index >= 0)
    {
        m_ui->transitionToApp((size_t)index);
        return true;
    }
    return false;
}

void DisplayManager::updateAppVector(const char* json)
{
    /* parse JSON with "apps": ["Time","Date"...] and update APP_REGISTRY.apps() */
    cJSON* doc = cJSON_Parse(json);
    if (!doc) return;
    cJSON* arr = cJSON_GetObjectItem(doc, "apps");
    if (!arr || !cJSON_IsArray(arr))
    {
        cJSON_Delete(doc);
        return;
    }

    std::vector<std::pair<std::string, AppCallback>> apps;
    for (int i = 0; i < cJSON_GetArraySize(arr); i++)
    {
        cJSON* item = cJSON_GetArrayItem(arr, i);
        if (item && item->valuestring)
        {
            std::string n(item->valuestring);
            if (n == "Time") apps.push_back({"Time", TimeApp});
            else if (n == "Date") apps.push_back({"Date", DateApp});
            else if (n == "Temp") apps.push_back({"Temp", TempApp});
            else if (n == "Hum") apps.push_back({"Hum", HumApp});
            else if (n == "Bat") apps.push_back({"Bat", BatApp});
            else if (n == "Weather") apps.push_back({"Weather", WeatherApp});
            else if (APP_REGISTRY.findCustomApp(n))
            {
                /* Re-link an existing custom app to the shared renderer. */
                apps.push_back({n, renderCustomApp});
            }
            /* else: unknown name → silently skip rather than push nullptr. */
        }
    }
    Lock _l(&m_dataLock);
    APP_REGISTRY.replaceApps(std::move(apps));
    m_ui->setApps(APP_REGISTRY.apps());
    cJSON_Delete(doc);
}

void DisplayManager::reorderApps(const char* json)
{
    updateAppVector(json);
}

/* ── Notification ────────────────────────────────────────────── */
/* Helper: accept either an unsigned int (e.g. 16711680) or a hex string
 * ("#FF0000" / "FF0000") and return a 0xRRGGBB value. */
static uint32_t json_color(cJSON* v, uint32_t fallback)
{
    if (!v) return fallback;
    if (cJSON_IsNumber(v)) return (uint32_t)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring)
    {
        const char* s = v->valuestring;
        if (*s == '#') s++;
        return (uint32_t)strtoul(s, nullptr, 16);
    }
    return fallback;
}

bool DisplayManager::generateNotification(uint8_t source, const char* json)
{
    (void)source;
    cJSON* doc = cJSON_Parse(json);
    if (!doc) return false;

    AwtrixNotification notif;

    /* Required: text */
    cJSON* t = cJSON_GetObjectItem(doc, "text");
    if (t && cJSON_IsString(t)) notif.text = t->valuestring;

    /* Display timing */
    cJSON* d = cJSON_GetObjectItem(doc, "duration");
    if (d && cJSON_IsNumber(d)) notif.duration = d->valueint;

    cJSON* r = cJSON_GetObjectItem(doc, "repeat");
    if (r && cJSON_IsNumber(r)) notif.repeat = r->valueint;

    cJSON* hold = cJSON_GetObjectItem(doc, "hold");
    if (hold && cJSON_IsTrue(hold))
    {
        notif.noScrolling = true;
        notif.repeat = -1;
        notif.duration = 0; /* 0 → never time out */
    }

    cJSON* ns = cJSON_GetObjectItem(doc, "noScroll");
    if (ns && cJSON_IsTrue(ns)) notif.noScrolling = true;

    /* Colors */
    notif.color = json_color(cJSON_GetObjectItem(doc, "color"), CONFIG.textColor888);
    notif.bgColor = json_color(cJSON_GetObjectItem(doc, "background"), 0);

    /* Background effect: accept either index or name */
    cJSON* eff = cJSON_GetObjectItem(doc, "effect");
    if (eff)
    {
        if (cJSON_IsNumber(eff)) notif.effect = eff->valueint;
        else if (cJSON_IsString(eff)) notif.effect = getEffectIndex(eff->valuestring);
    }

    /* Sound: original uses "sound" for SD player + "rtttl" for melody string.
     * We map both onto the in-buzzer RTTTL player. */
    cJSON* rt = cJSON_GetObjectItem(doc, "rtttl");
    if (rt && cJSON_IsString(rt) && rt->valuestring && *rt->valuestring)
    {
        notif.rtttl = true;
        notif.sound = rt->valuestring;
    }

    /* Stacking */
    cJSON* st = cJSON_GetObjectItem(doc, "stack");
    if (st && cJSON_IsBool(st)) notif.stack = cJSON_IsTrue(st);

    /* Wakeup / push flags (kept for parity with the original API) */
    cJSON* wk = cJSON_GetObjectItem(doc, "wakeup");
    if (wk && cJSON_IsBool(wk)) notif.wakeup = cJSON_IsTrue(wk);
    cJSON* pu = cJSON_GetObjectItem(doc, "push");
    if (pu && cJSON_IsBool(pu)) notif.push = cJSON_IsTrue(pu);

    /* ── Pack G: full notification field set (text styling + chart + icon).
     * Mirrors src/DisplayManager.cpp::generateNotification keys. */
    cJSON* cv;
    if ((cv = cJSON_GetObjectItem(doc, "center")) && cJSON_IsBool(cv)) notif.center = cJSON_IsTrue(cv);
    if ((cv = cJSON_GetObjectItem(doc, "rainbow")) && cJSON_IsBool(cv)) notif.rainbow = cJSON_IsTrue(cv);
    if ((cv = cJSON_GetObjectItem(doc, "scrollSpeed")) && cJSON_IsNumber(cv)) notif.scrollSpeed = cv->valueint;
    if ((cv = cJSON_GetObjectItem(doc, "pushIcon")) && cJSON_IsNumber(cv)) notif.pushIcon = cv->valueint;
    if ((cv = cJSON_GetObjectItem(doc, "iconOffset")) && cJSON_IsNumber(cv)) notif.iconOffset = cv->valueint;
    if ((cv = cJSON_GetObjectItem(doc, "textCase")) && cJSON_IsNumber(cv)) notif.textCase = cv->valueint;
    if ((cv = cJSON_GetObjectItem(doc, "textOffset")) && cJSON_IsNumber(cv)) notif.textOffset = cv->valueint;
    if ((cv = cJSON_GetObjectItem(doc, "topText")) && cJSON_IsBool(cv)) notif.topText = cJSON_IsTrue(cv);
    /* Both fade/blink and original fadeText/blinkText accepted (key alias). */
    if ((cv = cJSON_GetObjectItem(doc, "fadeText")) && cJSON_IsNumber(cv)) notif.fadeText = cv->valueint;
    if ((cv = cJSON_GetObjectItem(doc, "fade")) && cJSON_IsNumber(cv)) notif.fadeText = cv->valueint;
    if ((cv = cJSON_GetObjectItem(doc, "blinkText")) && cJSON_IsNumber(cv)) notif.blinkText = cv->valueint;
    if ((cv = cJSON_GetObjectItem(doc, "blink")) && cJSON_IsNumber(cv)) notif.blinkText = cv->valueint;
    if ((cv = cJSON_GetObjectItem(doc, "loopSound")) && cJSON_IsBool(cv)) notif.loopSound = cJSON_IsTrue(cv);

    /* Progress bar */
    if ((cv = cJSON_GetObjectItem(doc, "progress")) && cJSON_IsNumber(cv)) notif.progress = cv->valueint;
    notif.progressC = json_color(cJSON_GetObjectItem(doc, "progressC"), 0x00FF00);
    notif.progressBC = json_color(cJSON_GetObjectItem(doc, "progressBC"), 0x202020);

    /* gradient[2] = [startHex, endHex] (numbers or hex strings) */
    cJSON* grad = cJSON_GetObjectItem(doc, "gradient");
    if (grad && cJSON_IsArray(grad) && cJSON_GetArraySize(grad) >= 2)
    {
        notif.gradient[0] = json_color(cJSON_GetArrayItem(grad, 0), 0);
        notif.gradient[1] = json_color(cJSON_GetArrayItem(grad, 1), 0);
    }

    /* bar / line data arrays (up to 16 points). */
    auto load_int_array = [](cJSON* arr, std::vector<int>& out)
    {
        out.clear();
        if (!arr || !cJSON_IsArray(arr)) return;
        int n = cJSON_GetArraySize(arr);
        if (n > 16) n = 16;
        for (int i = 0; i < n; ++i)
        {
            cJSON* e = cJSON_GetArrayItem(arr, i);
            if (e && cJSON_IsNumber(e)) out.push_back(e->valueint);
        }
    };
    load_int_array(cJSON_GetObjectItem(doc, "bar"), notif.bar);
    load_int_array(cJSON_GetObjectItem(doc, "line"), notif.line);
    /* Accept both new-style barBC and original-style barBG (key alias). */
    notif.barBC = json_color(cJSON_GetObjectItem(doc, "barBC"), 0);
    if (notif.barBC == 0) notif.barBC = json_color(cJSON_GetObjectItem(doc, "barBG"), 0);
    if ((cv = cJSON_GetObjectItem(doc, "autoscale")) && cJSON_IsBool(cv)) notif.autoscale = cJSON_IsTrue(cv);

    /* Icon: SPIFFS-relative name or "BASE64,xxxx"; renderer in pack C decides. */
    if ((cv = cJSON_GetObjectItem(doc, "icon")) && cJSON_IsString(cv) && cv->valuestring)
    {
        notif.iconName = cv->valuestring;
    }

    /* effectSettings: forwarded verbatim as JSON to the fx engine. */
    if ((cv = cJSON_GetObjectItem(doc, "effectSettings")))
    {
        char* raw = cJSON_PrintUnformatted(cv);
        if (raw)
        {
            notif.effectSettings = raw;
            cJSON_free(raw);
        }
    }

    /* text as array → fragments + colors. */
    if (t && cJSON_IsArray(t))
    {
        notif.text.clear();
        int n = cJSON_GetArraySize(t);
        for (int i = 0; i < n; ++i)
        {
            cJSON* frag = cJSON_GetArrayItem(t, i);
            if (!frag) continue;
            if (cJSON_IsString(frag) && frag->valuestring)
            {
                notif.fragments.push_back(frag->valuestring);
                notif.colors.push_back(notif.color);
                notif.text += frag->valuestring;
            }
            else if (cJSON_IsObject(frag))
            {
                cJSON* ftext = cJSON_GetObjectItem(frag, "t");
                cJSON* fcol = cJSON_GetObjectItem(frag, "c");
                std::string s = (ftext && cJSON_IsString(ftext) && ftext->valuestring) ? ftext->valuestring : "";
                notif.fragments.push_back(s);
                notif.colors.push_back(json_color(fcol, notif.color));
                notif.text += s;
            }
        }
    }

    /* Defer startTime: NotifyOverlay sets it on the first render frame so the
     * `duration` clock starts when the user actually sees the notification. */
    notif.startTime = 0;

    {
        Lock _l(&m_dataLock);
        if (notif.stack || NOTIFICATIONS.empty())
        {
            NOTIFICATIONS.enqueue(notif);
        }
        else
        {
            NOTIFICATIONS.replaceHead(notif); /* replace the head, mirrors original */
        }
    }

    ESP_LOGI(TAG, "Notification: \"%s\" dur=%d rtttl=%d effect=%d stack=%d",
             notif.text.c_str(), notif.duration, notif.rtttl ? 1 : 0,
             notif.effect, notif.stack ? 1 : 0);

    cJSON_Delete(doc);
    return true;
}

void DisplayManager::dismissNotify()
{
    Lock _l(&m_dataLock);
    NOTIFICATIONS.dismiss();
}

/* ── drawing conveniences ────────────────────────────────────── */
void DisplayManager::drawProgressBar(int16_t x, int16_t y,
                                     int progress, uint32_t pc, uint32_t bg)
{
    if (!m_matrix) return;
    int w = (progress * 28) / 100;
    m_matrix->fillRect(x, y, w, 5, pc);
    m_matrix->fillRect(x + w, y, 28 - w, 5, bg);
}

void DisplayManager::HSVtext(int16_t x, int16_t y, const char* text,
                             bool centered, uint8_t textCase)
{
    if (!m_matrix) return;
    if (centered)
    {
        int len = strlen(text);
        x = (32 - len * 4) / 2;
    }
    m_matrix->setCursor(x, y);
    m_matrix->setTextColor(CONFIG.textColor888);
    m_matrix->print(text);
    m_matrix->show();
    update_display_snapshot_from_matrix(m_matrix);
}

void DisplayManager::printText(int16_t x, int16_t y, const char* text,
                               bool centered, uint8_t textCase)
{
    if (!m_matrix) return;
    m_matrix->setCursor(x, y);
    m_matrix->setTextColor(CONFIG.textColor888);
    m_matrix->print(text);
}

/* ── Power / brightness ──────────────────────────────────────── */
void DisplayManager::setPower(bool on)
{
    m_matrixOff = !on;
    if (m_matrix) m_matrix->setBrightness(on ? CONFIG.brightness : 0);
    /* L1: tell the state machine. We allow rejection (e.g. ArtNet ↔ PoweredOff
     * is not a legal edge) — that just means the user issued /api/power off
     * while a higher-priority mode owns the matrix; the brightness=0 still
     * takes effect, but the macro state stays unchanged. */
    SYS_TRANSITION(on
                       ? awtrix::SystemState::Normal
                       : awtrix::SystemState::PoweredOff,
                   on ? "matrix_on" : "matrix_off");
}

void DisplayManager::setBrightness(int bri)
{
    if (m_matrix) m_matrix->setBrightness(m_matrixOff ? 0 : bri);
}

bool DisplayManager::setAutoTransition(bool active)
{
    if (!m_ui) return false;
    if (m_ui->getAppCount() < 2)
    {
        m_ui->disableAutoTransition();
        return false;
    }
    if (active) m_ui->enableAutoTransition();
    else m_ui->disableAutoTransition();
    return true;
}

/* ── JSON exports ────────────────────────────────────────────── */
std::string DisplayManager::getAppsAsJson() const
{
    Lock _l(&m_dataLock);
    cJSON* root = cJSON_CreateArray();
    for (auto& a : APP_REGISTRY.apps()) cJSON_AddItemToArray(root, cJSON_CreateString(a.first.c_str()));
    char* s = cJSON_PrintUnformatted(root);
    std::string res(s ? s : "[]");
    if (s) cJSON_free(s);
    cJSON_Delete(root);
    return res;
}

std::string DisplayManager::getSettings() const
{
    return CONFIG.toJson();
}

std::string DisplayManager::getStats() const
{
    AwtrixConfig::Guard guard(CONFIG);
    cJSON* r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "uptime", _now() / 1000);
    cJSON_AddNumberToObject(r, "received_messages", CONFIG.receivedMessages);
    cJSON_AddStringToObject(r, "version", CONFIG.version ? CONFIG.version : "");
    char* s = cJSON_PrintUnformatted(r);
    std::string res(s ? s : "{}");
    if (s) cJSON_free(s);
    cJSON_Delete(r);
    return res;
}

std::string DisplayManager::ledsAsJson() const
{
    Lock _l(&m_dataLock);
    if (!m_matrix) return "[]";
    const CRGB* leds = m_matrix->getLeds();
    int total = m_matrix->width() * m_matrix->height();
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < total; i++)
    {
        int color = ((int)leds[i].r << 16) | ((int)leds[i].g << 8) | (int)leds[i].b;
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(color));
    }
    char* s = cJSON_PrintUnformatted(arr);
    std::string res(s ? s : "[]");
    if (s) cJSON_free(s);
    cJSON_Delete(arr);
    return res;
}

/* ── Indicator parser ────────────────────────────────────────── */
bool DisplayManager::indicatorParser(uint8_t ind, const char* json)
{
    cJSON* doc = cJSON_Parse(json);
    if (!doc) return false;
    cJSON* c = cJSON_GetObjectItem(doc, "color");
    cJSON* s = cJSON_GetObjectItem(doc, "state");
    if (!m_ui)
    {
        cJSON_Delete(doc);
        return false;
    }
    uint32_t col = 0xFF0000;
    if (c) col = (uint32_t)strtoul(c->valuestring ? c->valuestring : "FF0000", nullptr, 16);
    bool on = s ? cJSON_IsTrue(s) : true;
    switch (ind)
    {
    case 1: m_ui->setIndicator1Color(col);
        m_ui->setIndicator1State(on);
        break;
    case 2: m_ui->setIndicator2Color(col);
        m_ui->setIndicator2State(on);
        break;
    case 3: m_ui->setIndicator3Color(col);
        m_ui->setIndicator3State(on);
        break;
    }
    cJSON_Delete(doc);
    return true;
}

/* Persistent moodlight state used by tick() to keep painting between calls
 * lives at the top of this file (forward-decl block). */

bool DisplayManager::moodlight(const char* json)
{
    if (!json || !*json)
    {
        {
            Lock _l(&m_dataLock);
            AwtrixConfig::Guard cfgGuard(CONFIG);
            s_moodlightActive = false;
            CONFIG.moodlightMode = false;
        }
        ESP_LOGI(TAG, "Moodlight: off");
        /* L1: leave moodlight → back to Normal. Rejected if we're already
         * in a non-Moodlight state, which is fine — the flag flip above
         * is the source of truth, the state machine just mirrors it. */
        SYS_TRANSITION(awtrix::SystemState::Normal, "moodlight_off");
        return true;
    }
    cJSON* doc = cJSON_Parse(json);
    if (!doc) return false;

    /* Empty object → turn off. */
    if (cJSON_GetArraySize(doc) == 0)
    {
        cJSON_Delete(doc);
        {
            Lock _l(&m_dataLock);
            AwtrixConfig::Guard cfgGuard(CONFIG);
            s_moodlightActive = false;
            CONFIG.moodlightMode = false;
        }
        ESP_LOGI(TAG, "Moodlight: off");
        SYS_TRANSITION(awtrix::SystemState::Normal, "moodlight_off_empty");
        return true;
    }

    /* Color: hex string, integer or {"kelvin":n}. */
    cJSON* col = cJSON_GetObjectItem(doc, "color");
    cJSON* bri = cJSON_GetObjectItem(doc, "brightness");
    cJSON* kel = cJSON_GetObjectItem(doc, "kelvin");

    uint32_t rgb = 0xFFFFFF;
    if (col)
    {
        rgb = json_color(col, 0xFFFFFF);
    }
    else if (kel && cJSON_IsNumber(kel))
    {
        /* Approximate kelvin → RGB using the Tanner Helland piecewise model. */
        double k = kel->valuedouble / 100.0;
        double r, g, b;
        if (k <= 66)
        {
            r = 255;
            g = 99.4708025861 * log(k) - 161.1195681661;
            b = (k <= 19) ? 0 : (138.5177312231 * log(k - 10) - 305.0447927307);
        }
        else
        {
            r = 329.698727446 * pow(k - 60, -0.1332047592);
            g = 288.1221695283 * pow(k - 60, -0.0755148492);
            b = 255;
        }
        auto cl = [](double v)
        {
            if (v < 0) return 0;
            if (v > 255) return 255;
            return (int)v;
        };
        rgb = ((uint32_t)cl(r) << 16) | ((uint32_t)cl(g) << 8) | (uint32_t)cl(b);
    }

    /* Optional brightness override (0-255). When omitted we keep CONFIG.brightness. */
    if (bri && cJSON_IsNumber(bri) && m_matrix)
    {
        int v = bri->valueint;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        m_matrix->setBrightness(v);
    }

    cJSON_Delete(doc);

    /* Atomic state flip — tick() may race on these vars. */
    {
        Lock _l(&m_dataLock);
        AwtrixConfig::Guard cfgGuard(CONFIG);
        s_moodlightColor = rgb;
        s_moodlightActive = true;
        CONFIG.moodlightMode = true;
    }
    ESP_LOGI(TAG, "Moodlight: 0x%06X", (unsigned)(rgb & 0xFFFFFF));
    /* L1: enter Moodlight mode. */
    SYS_TRANSITION(awtrix::SystemState::Moodlight, "moodlight_on");

    /* Paint immediately so the user sees the change without waiting for tick(). */
    if (m_matrix)
    {
        m_matrix->fillRect(0, 0, m_matrix->width(), m_matrix->height(), rgb);
        m_matrix->show();
        update_display_snapshot_from_matrix(m_matrix);
    }
    return true;
}

/* ── Custom app rendering ───────────────────────────────────────
 * Minimal viable implementation. Each custom app stores a text + color
 * in AppRegistry (keyed by app name). Every custom slot in APP_REGISTRY.apps()
 * shares the same renderer lambda which looks the data
 * back up by name from the UiState::userData pointer.
 *
 * Pack B (round 7) full re-port: this is now a faithful translation of
 * src/Apps.cpp::ShowCustomApp (~310 lines) covering text scrolling,
 * rainbow/gradient/fragments coloring, autoscaled bar/line charts,
 * progress, drawInstructions DSL, lifeTimeEnd red border, {{placeholder}}
 * substitution and repeat counters. Icon rendering still falls back to
 * the GIF decoder which is delivered in pack K.
 *
 * A1 update: the four file-static helpers that used to live here
 * (custom_hsv_to_rgb / custom_lerp_color / custom_apply_fx /
 * custom_scale_chart) are now in <awtrix_color_utils.h> as
 * `awtrix_color::*` so notification + customApp + Functions share one
 * definition. custom_replace_placeholders stays here because it touches
 * core-specific CONFIG fields the kernel layer can't see.
 *
 * B2 update: `renderCustomApp` and `custom_replace_placeholders` themselves
 * now live in DisplayManager_customApp.cpp. The forward declaration at the
 * top of this file is enough for APP_REGISTRY references; the linker pulls
 * the sibling TU at app-image link time.
 */

bool DisplayManager::parseCustomPage(const char* name, const char* json, bool preventSave)
{
    if (!name || !*name) return false;
    std::string sname(name);

    /* Empty payload → delete the custom app. */
    bool isEmpty = (!json || !*json || strcmp(json, "{}") == 0);
    if (isEmpty)
    {
        {
            Lock _l(&m_dataLock);
            APP_REGISTRY.eraseCustomApp(sname);
            APP_REGISTRY.eraseApp(sname);
            if (m_ui) m_ui->setApps(APP_REGISTRY.apps());
        }

        /* Best-effort delete of the on-disk JSON. SPIFFS unlink is not
         * shared mutable state, so it stays outside the critical section. */
        char path[160];
        snprintf(path, sizeof(path), "/spiffs/CUSTOMAPPS/%s.json", name);
        unlink(path);

        ESP_LOGI(TAG, "Custom app '%s' removed", name);
        return true;
    }

    cJSON* doc = cJSON_Parse(json);
    if (!doc)
    {
        ESP_LOGW(TAG, "Custom app '%s': invalid JSON", name);
        return false;
    }

    /* The original supports either a single object or an array of pages.
     * For minimal support we only accept the single-object form here. */
    if (!cJSON_IsObject(doc))
    {
        ESP_LOGW(TAG, "Custom app '%s': only single-object payloads supported", name);
        cJSON_Delete(doc);
        return false;
    }

    /* Parse the JSON into a stack-local CustomApp first; only after every
     * field has been populated do we swap it into the map under the data
     * lock. This shortens the critical section to ~microseconds and avoids
     * the previous race where a reference into m_customApps could be
     * invalidated mid-parse by a concurrent erase. */
    CustomApp app;
    app.name = sname;

    cJSON* t = cJSON_GetObjectItem(doc, "text");
    if (t && cJSON_IsString(t)) app.text = t->valuestring;

    cJSON* c = cJSON_GetObjectItem(doc, "color");
    if (c)
    {
        app.color = json_color(c, CONFIG.textColor888);
        app.hasCustomColor = true;
    }

    cJSON* bg = cJSON_GetObjectItem(doc, "background");
    if (bg) app.background = json_color(bg, 0);

    cJSON* eff = cJSON_GetObjectItem(doc, "effect");
    if (eff)
    {
        if (cJSON_IsNumber(eff)) app.effect = eff->valueint;
        else if (cJSON_IsString(eff)) app.effect = getEffectIndex(eff->valuestring);
    }

    cJSON* dur = cJSON_GetObjectItem(doc, "duration");
    if (dur && cJSON_IsNumber(dur)) app.duration = (long)dur->valuedouble * 1000L;

    cJSON* lt = cJSON_GetObjectItem(doc, "lifetime");
    if (lt && cJSON_IsNumber(lt)) app.lifetime = (uint64_t)lt->valuedouble;

    cJSON* ltm = cJSON_GetObjectItem(doc, "lifetimeMode");
    if (ltm && cJSON_IsNumber(ltm)) app.lifetimeMode = (uint8_t)ltm->valueint;

    cJSON* rb = cJSON_GetObjectItem(doc, "rainbow");
    if (rb && cJSON_IsBool(rb)) app.rainbow = cJSON_IsTrue(rb);

    cJSON* ce = cJSON_GetObjectItem(doc, "center");
    if (ce && cJSON_IsBool(ce)) app.center = cJSON_IsTrue(ce);

    cJSON* ns = cJSON_GetObjectItem(doc, "noScroll");
    if (ns && cJSON_IsBool(ns)) app.noScrolling = cJSON_IsTrue(ns);

    cJSON* ic = cJSON_GetObjectItem(doc, "icon");
    if (ic && cJSON_IsString(ic)) app.iconName = ic->valuestring;

    /* ── Extended fields (mirrors the full original parseCustomPage) ── */
    cJSON* tc = cJSON_GetObjectItem(doc, "textCase");
    if (tc && cJSON_IsNumber(tc)) app.textCase = tc->valueint;

    cJSON* tt = cJSON_GetObjectItem(doc, "topText");
    if (tt && cJSON_IsBool(tt)) app.topText = cJSON_IsTrue(tt);

    cJSON* toff = cJSON_GetObjectItem(doc, "textOffset");
    if (toff && cJSON_IsNumber(toff)) app.textOffset = toff->valueint;

    cJSON* ss = cJSON_GetObjectItem(doc, "scrollSpeed");
    if (ss && cJSON_IsNumber(ss)) app.scrollSpeed = (float)ss->valuedouble;

    cJSON* fd = cJSON_GetObjectItem(doc, "fade");
    if (fd && cJSON_IsNumber(fd)) app.fade = fd->valueint;

    cJSON* bk = cJSON_GetObjectItem(doc, "blink");
    if (bk && cJSON_IsNumber(bk)) app.blink = bk->valueint;

    cJSON* grd = cJSON_GetObjectItem(doc, "gradient");
    if (grd && cJSON_IsArray(grd) && cJSON_GetArraySize(grd) >= 2)
    {
        app.gradient[0] = json_color(cJSON_GetArrayItem(grd, 0), 0);
        app.gradient[1] = json_color(cJSON_GetArrayItem(grd, 1), 0);
    }

    cJSON* pi = cJSON_GetObjectItem(doc, "pushIcon");
    if (pi && cJSON_IsNumber(pi)) app.pushIcon = (uint8_t)pi->valueint;

    /* Pack H: extra fields ported from the original generateCustomPage that
     * were missing in earlier rounds. They cover icon offset, effect
     * passthrough, chart autoscale toggle, lifetime/lifetimeMode (consumed
     * by Pack J's checkLifetime), bounce text and the explicit save flag. */
    cJSON* ioff = cJSON_GetObjectItem(doc, "iconOffset");
    if (ioff && cJSON_IsNumber(ioff)) app.iconOffset = ioff->valueint;

    cJSON* eff_s = cJSON_GetObjectItem(doc, "effectSettings");
    if (eff_s)
    {
        char* raw = cJSON_PrintUnformatted(eff_s);
        if (raw)
        {
            app.effectSettings = raw;
            cJSON_free(raw);
        }
    }

    cJSON* aut = cJSON_GetObjectItem(doc, "autoscale");
    if (aut && cJSON_IsBool(aut)) app.autoscale = cJSON_IsTrue(aut);

    cJSON* bnc = cJSON_GetObjectItem(doc, "bounce");
    if (bnc && cJSON_IsBool(bnc)) app.bounce = cJSON_IsTrue(bnc);

    /* "save" overrides the preventSave caller knob (lets HTTP POST opt out
     * of persistence even without the API-level preventSave flag). */
    cJSON* sv = cJSON_GetObjectItem(doc, "save");
    if (sv && cJSON_IsBool(sv) && !cJSON_IsTrue(sv)) preventSave = true;

    cJSON* rp = cJSON_GetObjectItem(doc, "repeat");
    if (rp && cJSON_IsNumber(rp)) app.repeat = rp->valueint;

    /* Vector-based ops: draw / bar / line / progress. */
    cJSON* di = cJSON_GetObjectItem(doc, "draw");
    if (di)
    {
        char* s = cJSON_PrintUnformatted(di);
        if (s)
        {
            app.drawInstructions = s;
            free(s);
            /* Pre-compile to the POD opcode vector NOW, so the 60 Hz render
             * never needs to cJSON_Parse the same string again. */
            compileDrawInstructions(app.drawInstructions.c_str(),
                                    app.compiledDraw, app.drawTexts);
        }
    }

    cJSON* barj = cJSON_GetObjectItem(doc, "bar");
    if (barj && cJSON_IsArray(barj))
    {
        int n = cJSON_GetArraySize(barj);
        if (n > 16) n = 16;
        app.barSize = n;
        for (int i = 0; i < n; i++)
        {
            cJSON* v = cJSON_GetArrayItem(barj, i);
            app.barData[i] = v ? v->valueint : 0;
        }
    }
    cJSON* barbg = cJSON_GetObjectItem(doc, "barBG");
    if (barbg) app.barBG = json_color(barbg, 0);

    cJSON* linej = cJSON_GetObjectItem(doc, "line");
    if (linej && cJSON_IsArray(linej))
    {
        int n = cJSON_GetArraySize(linej);
        if (n > 16) n = 16;
        app.lineSize = n;
        for (int i = 0; i < n; i++)
        {
            cJSON* v = cJSON_GetArrayItem(linej, i);
            app.lineData[i] = v ? v->valueint : 0;
        }
    }

    cJSON* prg = cJSON_GetObjectItem(doc, "progress");
    if (prg && cJSON_IsNumber(prg)) app.progress = prg->valueint;
    cJSON* pc = cJSON_GetObjectItem(doc, "progressC");
    if (pc) app.pColor = json_color(pc, 0x00FF00);
    cJSON* pbc = cJSON_GetObjectItem(doc, "progressBC");
    if (pbc) app.pbColor = json_color(pbc, 0x202020);

    /* Multi-color fragments (mirrors fragments + colors arrays in original). */
    cJSON* frg = cJSON_GetObjectItem(doc, "fragments");
    if (frg && cJSON_IsArray(frg))
    {
        app.fragments.clear();
        app.colors.clear();
        int n = cJSON_GetArraySize(frg);
        for (int i = 0; i < n; i++)
        {
            cJSON* fr = cJSON_GetArrayItem(frg, i);
            if (!fr) continue;
            cJSON* t = cJSON_GetObjectItem(fr, "t");
            cJSON* c = cJSON_GetObjectItem(fr, "c");
            app.fragments.push_back(t && cJSON_IsString(t) ? t->valuestring : "");
            app.colors.push_back(c ? json_color(c, app.color) : app.color);
        }
    }

    app.lastUpdate = (long)_now();
    cJSON_Delete(doc);

    /* Atomic swap-in + APP_REGISTRY.apps() insertion under the data lock. */
    {
        Lock _l(&m_dataLock);
        APP_REGISTRY.upsertCustomApp(sname, std::move(app));

        auto eIt = std::find_if(APP_REGISTRY.apps().begin(), APP_REGISTRY.apps().end(),
                                [&](const std::pair<std::string, AppCallback>& p) { return p.first == sname; });
        if (eIt == APP_REGISTRY.apps().end())
        {
            APP_REGISTRY.apps().push_back({sname, renderCustomApp});
        }
        else
        {
            eIt->second = renderCustomApp;
        }
        if (m_ui) m_ui->setApps(APP_REGISTRY.apps());
    }

    /* Persist unless told not to (e.g. when called from loadCustomApps). */
    if (!preventSave)
    {
        mkdir("/spiffs/CUSTOMAPPS", 0775);
        char path[160];
        snprintf(path, sizeof(path), "/spiffs/CUSTOMAPPS/%s.json", name);
        FILE* fp = fopen(path, "w");
        if (fp)
        {
            fwrite(json, 1, strlen(json), fp);
            fclose(fp);
        }
        else
        {
            ESP_LOGW(TAG, "Custom app '%s': could not persist to %s", name, path);
        }
    }

    /* P1-D: scan the rendered text + every fragment for {{topic}} markers.
     * Each non-CONFIG.* topic is registered with the placeholder cache and
     * MQTT-subscribed (via the weak symbol — no-op if MQTT is disabled).
     * Subscribing only once per topic is enforced by the placeholder
     * registry (the second register() call on the same topic is a no-op).
     * Mirrors src/Functions.cpp::subscribeToPlaceholders in the original. */
    auto scan_placeholders = [](const std::string& s)
    {
        size_t i = 0;
        while (i < s.size())
        {
            size_t open = s.find("{{", i);
            if (open == std::string::npos) break;
            size_t close = s.find("}}", open + 2);
            if (close == std::string::npos) break;
            std::string key = s.substr(open + 2, close - (open + 2));
            i = close + 2;
            if (key.empty()) continue;
            if (key.compare(0, 7, "CONFIG.") == 0) continue; /* static fallback */
            /* Idempotent: registry caps at 32 topics, MQTT subscribe is
             * deduplicated inside awtrix_mqtt::s_pending_subs. */
            if (awtrix_placeholder_register(key))
            {
                if (&awtrix_mqtt_subscribe) awtrix_mqtt_subscribe(key.c_str());
                ESP_LOGI(TAG, "Registered MQTT placeholder for '%s'", key.c_str());
            }
        }
    };
    {
        /* Re-fetch the just-upserted app under the lock so the scan sees
         * the final, persisted text/fragments (and so a concurrent delete
         * can't free the strings mid-iteration). */
        Lock _l(&m_dataLock);
        if (const CustomApp* up = APP_REGISTRY.findCustomApp(sname))
        {
            scan_placeholders(up->text);
            for (const auto& f : up->fragments) scan_placeholders(f);
        }
    }

    ESP_LOGI(TAG, "Custom app '%s' upserted (text=\"%s\")", name, app.text.c_str());
    return true;
}

/* Read-only accessor used by renderCustomApp() above. */
const std::map<std::string, CustomApp>& DisplayManager::peekCustomApps() const
{
    return APP_REGISTRY.customApps();
}

/* ═══════════════════════════════════════════════════════════════
 * Extended API surface (mirrors the original src/DisplayManager.cpp
 * methods that were not part of the initial Phase-3 port).
 * ═════════════════════════════════════════════════════════════ */

/* ── extra drawing helpers ──────────────────────────────────── */
void DisplayManager::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t c)
{
    if (m_matrix) m_matrix->drawRect(x, y, w, h, c);
}

void DisplayManager::drawFilledRect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t c)
{
    if (m_matrix) m_matrix->fillRect(x, y, w, h, c);
}

void DisplayManager::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t c)
{
    if (m_matrix) m_matrix->drawLine(x0, y0, x1, y1, c);
}

void DisplayManager::drawPixel(int16_t x, int16_t y, uint32_t c)
{
    if (m_matrix) m_matrix->drawPixel(x, y, c);
}

void DisplayManager::drawCircle(int16_t x0, int16_t y0, int16_t r, uint32_t c)
{
    if (m_matrix) m_matrix->drawCircle(x0, y0, r, c);
}

void DisplayManager::fillCircle(int16_t x0, int16_t y0, int16_t r, uint32_t c)
{
    if (m_matrix) m_matrix->fillCircle(x0, y0, r, c);
}

void DisplayManager::drawFastVLine(int16_t x, int16_t y, int16_t h, uint32_t c)
{
    if (m_matrix) m_matrix->drawFastVLine(x, y, h, c);
}

void DisplayManager::drawRGBBitmap(int16_t x, int16_t y, const uint32_t* bmp,
                                   int16_t w, int16_t h)
{
    if (m_matrix) m_matrix->drawRGBBitmap(x, y, bmp, w, h);
}

void DisplayManager::drawBMP(int16_t x, int16_t y, const uint16_t* bmp,
                             int16_t w, int16_t h)
{
    if (m_matrix) m_matrix->drawRGBBitmap(x, y, bmp, w, h);
}

/* JPEG drawing: the original used TJpg_Decoder. ESP-IDF v6 has esp_jpeg
 * available via the managed_component, but it is heavy. To preserve API
 * surface without dragging in a decoder we fall back to a no-op draw and
 * log a single info message. SPIFFS-stored decoded RGB565 bitmaps go through
 * drawBMP/drawRGBBitmap above. */
void DisplayManager::drawJPG(int16_t x, int16_t y, const uint8_t* data, uint32_t size)
{
    (void)x;
    (void)y;
    (void)data;
    (void)size;
    static bool warned = false;
    if (!warned)
    {
        ESP_LOGW(TAG, "drawJPG called but no JPEG decoder linked; use drawBMP/RGB.");
        warned = true;
    }
}

/* Bar/Line chart: scale `size` values into the 32x8 (or available) area
 * and draw rectangles/line segments. */
void DisplayManager::drawBarChart(int16_t x, int16_t y, const int* data, uint8_t n,
                                  bool withIcon, uint32_t color, uint32_t barBG)
{
    if (!m_matrix || !data || n == 0) return;
    int xoff = withIcon ? 8 : 0;
    int availW = m_matrix->width() - x - xoff;
    int availH = m_matrix->height();
    if (availW <= 0 || availH <= 0) return;
    int barW = availW / n;
    if (barW < 1) barW = 1;
    int maxv = 1;
    for (int i = 0; i < n; i++) if (data[i] > maxv) maxv = data[i];
    for (int i = 0; i < n; i++)
    {
        int h = (data[i] * availH) / maxv;
        if (h > availH) h = availH;
        int bx = x + xoff + i * barW;
        m_matrix->fillRect(bx, y, barW, availH - h, barBG);
        m_matrix->fillRect(bx, y + (availH - h), barW, h, color);
    }
}

void DisplayManager::drawLineChart(int16_t x, int16_t y, const int* data, uint8_t n,
                                   bool withIcon, uint32_t color)
{
    if (!m_matrix || !data || n < 2) return;
    int xoff = withIcon ? 8 : 0;
    int availW = m_matrix->width() - x - xoff;
    int availH = m_matrix->height();
    if (availW <= 0 || availH <= 0) return;
    int maxv = 1;
    for (int i = 0; i < n; i++) if (data[i] > maxv) maxv = data[i];
    for (int i = 1; i < n; i++)
    {
        int x0 = x + xoff + ((i - 1) * availW) / (n - 1);
        int x1 = x + xoff + (i * availW) / (n - 1);
        int y0 = y + availH - (data[i - 1] * availH) / maxv;
        int y1 = y + availH - (data[i] * availH) / maxv;
        m_matrix->drawLine(x0, y0, x1, y1, color);
    }
}

void DisplayManager::drawMenuIndicator(int cur, int total, uint32_t color)
{
    if (!m_matrix || total <= 0) return;
    int w = m_matrix->width();
    int seg = w / total;
    if (seg < 2) seg = 2;
    for (int i = 0; i < total; i++)
    {
        uint32_t c = (i == cur) ? color : 0x202020;
        m_matrix->fillRect(i * seg, m_matrix->height() - 1, seg - 1, 1, c);
    }
}

/* Extended JSON DSL for drawInstructions, mirrors the original
 * AWTRIX3 DisplayManager::processDrawInstructions (DisplayManager.cpp:1926+):
 *   [{"db":[x,y,w,h,col]},          filled rect
 *    {"dl":[x0,y0,x1,y1,col]},      line
 *    {"dp":[x,y,col]},              pixel
 *    {"dr":[x,y,w,h,col]},          outline rect
 *    {"dre":[x,y,w,h,col]},         outline rect (alias)
 *    {"dfre":[x,y,w,h,col]},        filled rect (alias)
 *    {"dc":[x,y,r,col]},            outline circle
 *    {"dci":[x,y,r,col]},           outline circle (alias)
 *    {"df":[x,y,r,col]},            filled circle
 *    {"dfc":[x,y,r,col]},           filled circle (alias)
 *    {"dt":[x,y,"text",col]},       text
 *    {"dlp":[c1,c2,c3,...]},        load palette of up to 16 colors
 *    {"dpa":[x,y,paletteIndex]}]    pixel-from-palette
 */
void DisplayManager::processDrawInstructions(int16_t x, int16_t y, const char* drawInstructions)
{
    if (!m_matrix || !drawInstructions || !*drawInstructions) return;
    cJSON* doc = cJSON_Parse(drawInstructions);
    if (!doc || !cJSON_IsArray(doc))
    {
        if (doc) cJSON_Delete(doc);
        return;
    }

    static uint32_t palette[16] = {0};
    static int paletteSize = 0;

    int n = cJSON_GetArraySize(doc);
    for (int i = 0; i < n; i++)
    {
        cJSON* item = cJSON_GetArrayItem(doc, i);
        if (!item || !cJSON_IsObject(item)) continue;
        cJSON* op = item->child;
        if (!op || !op->string) continue;
        cJSON* a = op;
        if (!cJSON_IsArray(a)) continue;
        int alen = cJSON_GetArraySize(a);
        auto ai = [&](int k)
        {
            cJSON* v = cJSON_GetArrayItem(a, k);
            return v ? v->valueint : 0;
        };
        auto au = [&](int k)
        {
            cJSON* v = cJSON_GetArrayItem(a, k);
            if (!v) return (uint32_t)0;
            if (cJSON_IsNumber(v)) return (uint32_t)v->valuedouble;
            if (cJSON_IsString(v) && v->valuestring)
            {
                const char* s = v->valuestring;
                if (*s == '#') s++;
                return (uint32_t)strtoul(s, nullptr, 16);
            }
            return (uint32_t)0;
        };
        const char* o = op->string;
        if ((!strcmp(o, "db") || !strcmp(o, "dfre")) && alen >= 5)
            m_matrix->fillRect(x + ai(0), y + ai(1), ai(2), ai(3), au(4));
        else if ((!strcmp(o, "dr") || !strcmp(o, "dre")) && alen >= 5)
            m_matrix->drawRect(x + ai(0), y + ai(1), ai(2), ai(3), au(4));
        else if (!strcmp(o, "dl") && alen >= 5)
            m_matrix->drawLine(x + ai(0), y + ai(1), x + ai(2), y + ai(3), au(4));
        else if (!strcmp(o, "dp") && alen >= 3)
            m_matrix->drawPixel(x + ai(0), y + ai(1), au(2));
        else if ((!strcmp(o, "dc") || !strcmp(o, "dci")) && alen >= 4)
            m_matrix->drawCircle(x + ai(0), y + ai(1), ai(2), au(3));
        else if ((!strcmp(o, "df") || !strcmp(o, "dfc")) && alen >= 4)
            m_matrix->fillCircle(x + ai(0), y + ai(1), ai(2), au(3));
        else if (!strcmp(o, "dt") && alen >= 4)
        {
            cJSON* tx = cJSON_GetArrayItem(a, 2);
            if (tx && cJSON_IsString(tx))
            {
                m_matrix->setTextColor(au(3));
                m_matrix->setCursor(x + ai(0), y + ai(1));
                m_matrix->print(tx->valuestring);
            }
        }
        else if (!strcmp(o, "dlp"))
        {
            paletteSize = alen < 16 ? alen : 16;
            for (int k = 0; k < paletteSize; k++) palette[k] = au(k);
        }
        else if (!strcmp(o, "dpa") && alen >= 3)
        {
            int idx = ai(2);
            if (idx >= 0 && idx < paletteSize)
                m_matrix->drawPixel(x + ai(0), y + ai(1), palette[idx]);
        }
    }
    cJSON_Delete(doc);
}

/* ── drawInstructions → POD compiler ─────────────────────────────
 * Parses the same JSON DSL as processDrawInstructions but produces a
 * POD `std::vector<DrawOp>` cached on CustomApp. The 60 Hz render path
 * then consumes the POD vector directly, avoiding cJSON_Parse per frame.
 *
 * Encoding choices:
 *   - args are clamped to int16; the matrix is 32x8 so this is plenty.
 *   - color is stored once per op as RGB888 (no '#FFAA00' string parsing
 *     in the hot path).
 *   - text operands live in a small `outTexts` table; the op carries the
 *     index. We deliberately don't use a single std::string per op so
 *     copying CustomApp by value (as renderCustomApp does under the lock)
 *     stays cheap.
 *   - OPC_PALETTE may have more than 4 colors; for that case we emit
 *     consecutive OPC_PALETTE ops with paletteSize<=4 each (the executor
 *     concatenates them). 16 colors → 4 palette ops. */
bool DisplayManager::compileDrawInstructions(const char* json,
                                             std::vector<DrawOp>& outOps,
                                             std::vector<std::string>& outTexts)
{
    outOps.clear();
    outTexts.clear();
    if (!json || !*json) return true; /* empty is OK — empty cache */

    cJSON* doc = cJSON_Parse(json);
    if (!doc || !cJSON_IsArray(doc))
    {
        if (doc) cJSON_Delete(doc);
        return false;
    }

    auto au = [](cJSON* v) -> uint32_t
    {
        if (!v) return 0;
        if (cJSON_IsNumber(v)) return (uint32_t)v->valuedouble;
        if (cJSON_IsString(v) && v->valuestring)
        {
            const char* s = v->valuestring;
            if (*s == '#') s++;
            return (uint32_t)strtoul(s, nullptr, 16);
        }
        return 0;
    };

    int n = cJSON_GetArraySize(doc);
    for (int i = 0; i < n; i++)
    {
        cJSON* item = cJSON_GetArrayItem(doc, i);
        if (!item || !cJSON_IsObject(item)) continue;
        cJSON* a = item->child; /* first (and only) object key */
        if (!a || !a->string || !cJSON_IsArray(a)) continue;
        int alen = cJSON_GetArraySize(a);
        auto ai = [&](int k)
        {
            cJSON* v = cJSON_GetArrayItem(a, k);
            return v ? (int16_t)v->valueint : (int16_t)0;
        };

        DrawOp d{};
        const char* o = a->string;

        if ((!strcmp(o, "db") || !strcmp(o, "dfre")) && alen >= 5)
        {
            d.op = OPC_FILLRECT;
            d.a[0] = ai(0);
            d.a[1] = ai(1);
            d.a[2] = ai(2);
            d.a[3] = ai(3);
            d.color = au(cJSON_GetArrayItem(a, 4));
        }
        else if ((!strcmp(o, "dr") || !strcmp(o, "dre")) && alen >= 5)
        {
            d.op = OPC_RECT;
            d.a[0] = ai(0);
            d.a[1] = ai(1);
            d.a[2] = ai(2);
            d.a[3] = ai(3);
            d.color = au(cJSON_GetArrayItem(a, 4));
        }
        else if (!strcmp(o, "dl") && alen >= 5)
        {
            d.op = OPC_LINE;
            d.a[0] = ai(0);
            d.a[1] = ai(1);
            d.a[2] = ai(2);
            d.a[3] = ai(3);
            d.color = au(cJSON_GetArrayItem(a, 4));
        }
        else if (!strcmp(o, "dp") && alen >= 3)
        {
            d.op = OPC_PIXEL;
            d.a[0] = ai(0);
            d.a[1] = ai(1);
            d.color = au(cJSON_GetArrayItem(a, 2));
        }
        else if ((!strcmp(o, "dc") || !strcmp(o, "dci")) && alen >= 4)
        {
            d.op = OPC_CIRCLE;
            d.a[0] = ai(0);
            d.a[1] = ai(1);
            d.a[2] = ai(2);
            d.color = au(cJSON_GetArrayItem(a, 3));
        }
        else if ((!strcmp(o, "df") || !strcmp(o, "dfc")) && alen >= 4)
        {
            d.op = OPC_FILLCIRCLE;
            d.a[0] = ai(0);
            d.a[1] = ai(1);
            d.a[2] = ai(2);
            d.color = au(cJSON_GetArrayItem(a, 3));
        }
        else if (!strcmp(o, "dt") && alen >= 4)
        {
            cJSON* tx = cJSON_GetArrayItem(a, 2);
            if (!tx || !cJSON_IsString(tx)) continue;
            d.op = OPC_TEXT;
            d.a[0] = ai(0);
            d.a[1] = ai(1);
            d.color = au(cJSON_GetArrayItem(a, 3));
            outTexts.push_back(tx->valuestring);
            d.textIdx = (uint8_t)(outTexts.size() - 1);
        }
        else if (!strcmp(o, "dlp"))
        {
            /* Split into OPC_PALETTE chunks of up to 4 palette entries each. */
            int total = alen < 16 ? alen : 16;
            for (int base = 0; base < total; base += 4)
            {
                DrawOp p{};
                p.op = OPC_PALETTE;
                p.paletteSize = (uint8_t)((total - base) < 4 ? (total - base) : 4);
                p.a[0] = (int16_t)base; /* palette base index */
                for (int k = 0; k < p.paletteSize; k++)
                {
                    p.palette[k] = au(cJSON_GetArrayItem(a, base + k));
                }
                outOps.push_back(p);
            }
            continue; /* already pushed */
        }
        else if (!strcmp(o, "dpa") && alen >= 3)
        {
            d.op = OPC_PALETTE_PIX;
            d.a[0] = ai(0);
            d.a[1] = ai(1);
            d.a[2] = ai(2); /* palette index */
        }
        else
        {
            continue; /* unknown opcode — skip */
        }
        outOps.push_back(d);
    }
    cJSON_Delete(doc);
    return true;
}

/* 60-Hz POD draw executor consumed by renderCustomApp. */
void DisplayManager::processCompiledDraw(int16_t x, int16_t y,
                                         const std::vector<DrawOp>& ops,
                                         const std::vector<std::string>& texts)
{
    if (!m_matrix || ops.empty()) return;
    /* Per-call palette state — keeping it on the stack makes
     * processCompiledDraw fully reentrant if a future caller (e.g. Artnet
     * preview) ever invokes it from a different task. */
    uint32_t palette[16] = {0};

    for (const DrawOp& d : ops)
    {
        switch (d.op)
        {
        case OPC_PIXEL:
            m_matrix->drawPixel(x + d.a[0], y + d.a[1], d.color);
            break;
        case OPC_LINE:
            m_matrix->drawLine(x + d.a[0], y + d.a[1],
                               x + d.a[2], y + d.a[3], d.color);
            break;
        case OPC_FILLRECT:
            m_matrix->fillRect(x + d.a[0], y + d.a[1], d.a[2], d.a[3], d.color);
            break;
        case OPC_RECT:
            m_matrix->drawRect(x + d.a[0], y + d.a[1], d.a[2], d.a[3], d.color);
            break;
        case OPC_FILLCIRCLE:
            m_matrix->fillCircle(x + d.a[0], y + d.a[1], d.a[2], d.color);
            break;
        case OPC_CIRCLE:
            m_matrix->drawCircle(x + d.a[0], y + d.a[1], d.a[2], d.color);
            break;
        case OPC_TEXT:
            if (d.textIdx < texts.size())
            {
                m_matrix->setTextColor(d.color);
                m_matrix->setCursor(x + d.a[0], y + d.a[1]);
                m_matrix->print(texts[d.textIdx].c_str());
            }
            break;
        case OPC_PALETTE:
            {
                int base = d.a[0];
                for (int k = 0; k < d.paletteSize; k++)
                {
                    int idx = base + k;
                    if (idx >= 0 && idx < 16) palette[idx] = d.palette[k];
                }
                break;
            }
        case OPC_PALETTE_PIX:
            {
                int idx = d.a[2];
                if (idx >= 0 && idx < 16)
                    m_matrix->drawPixel(x + d.a[0], y + d.a[1], palette[idx]);
                break;
            }
        default: break;
        }
    }
}

void DisplayManager::GradientText(int16_t x, int16_t y, const char* text,
                                  uint32_t c1, uint32_t c2, bool clearBG, uint8_t /*tc*/)
{
    if (!m_matrix || !text) return;
    if (clearBG) m_matrix->fillRect(x, y, m_matrix->width() - x, 8, 0);
    int len = (int)strlen(text);
    for (int i = 0; i < len; i++)
    {
        float t = (len > 1) ? (float)i / (float)(len - 1) : 0.0f;
        uint8_t r = (uint8_t)((((c1 >> 16) & 0xFF) * (1 - t)) + (((c2 >> 16) & 0xFF) * t));
        uint8_t g = (uint8_t)((((c1 >> 8) & 0xFF) * (1 - t)) + (((c2 >> 8) & 0xFF) * t));
        uint8_t b = (uint8_t)((((c1) & 0xFF) * (1 - t)) + (((c2) & 0xFF) * t));
        m_matrix->setTextColor(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
        m_matrix->setCursor(x + i * 4, y);
        char tmp[2] = {text[i], 0};
        m_matrix->print(tmp);
    }
}

/* ── low-level passthroughs ─────────────────────────────────── */
void DisplayManager::matrixPrint(const char* s) { if (m_matrix) m_matrix->print(s); }
void DisplayManager::matrixPrint(char c) { if (m_matrix) m_matrix->print(c); }
void DisplayManager::matrixPrint(double n, uint8_t d) { if (m_matrix) m_matrix->print(n, d); }
void DisplayManager::setCursor(int16_t x, int16_t y) { if (m_matrix) m_matrix->setCursor(x, y); }
void DisplayManager::setTextColor(uint32_t c) { if (m_matrix) m_matrix->setTextColor(c); }
void DisplayManager::resetTextColor() { if (m_matrix) m_matrix->setTextColor(CONFIG.textColor888); }
void DisplayManager::clearMatrix() { if (m_matrix) m_matrix->clear(); }
void DisplayManager::clear() { if (m_matrix) m_matrix->clear(); }

void DisplayManager::show()
{
    if (m_matrix)
    {
        m_matrix->show();
        update_display_snapshot_from_matrix(m_matrix);
    }
}

/* ── selectButton / selectButtonLong (forwarded to MenuManager when active) */
void DisplayManager::selectButton()
{
    /* MenuManager lives in awtrix_menumanager; we forward through a weak
     * symbol so this component doesn't depend on it at link time. */
    extern void awtrix_menu_select_short() __attribute__
    ((weak));
    if (&awtrix_menu_select_short) awtrix_menu_select_short();
    /* Otherwise dismiss the current notification. */
    dismissNotify();
}

void DisplayManager::selectButtonLong()
{
    extern void awtrix_menu_select_long() __attribute__
    ((weak));
    if (&awtrix_menu_select_long) awtrix_menu_select_long();
}

/* ── power state parse (accepts {"power":true/false}) ─────── */
void DisplayManager::powerStateParse(const char* json)
{
    if (!json) return;
    cJSON* doc = cJSON_Parse(json);
    if (!doc) return;
    cJSON* p = cJSON_GetObjectItem(doc, "power");
    if (p && cJSON_IsBool(p)) setPower(cJSON_IsTrue(p));
    cJSON_Delete(doc);
}

/* ── full-settings hot-update (best-effort: cherry-pick known keys) */
void DisplayManager::setNewSettings(const char* json)
{
    if (!json) return;
    cJSON* doc = cJSON_Parse(json);
    if (!doc) return;
    /* All CONFIG.* writes happen inside this critical section so concurrent
     * HTTP/MQTT/display/periphery readers never observe a half-applied batch. */
    {
        auto& c = CONFIG;
        AwtrixConfig::Guard configGuard(c);
        cJSON* v;
        if ((v = cJSON_GetObjectItem(doc, "BRI")) && cJSON_IsNumber(v)) c.brightness = v->valueint;
        if ((v = cJSON_GetObjectItem(doc, "ABRI")) && cJSON_IsBool(v)) c.auto_brightness = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "ATRANS")) && cJSON_IsBool(v)) c.auto_transition = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "ATIME")) && cJSON_IsNumber(v)) c.timePerApp = v->valuedouble;
        if ((v = cJSON_GetObjectItem(doc, "TSPEED")) && cJSON_IsNumber(v)) c.timePerTransition = v->valueint;
        if ((v = cJSON_GetObjectItem(doc, "TEFF")) && cJSON_IsNumber(v)) c.transEffect = v->valueint;
        if ((v = cJSON_GetObjectItem(doc, "BGE")) && cJSON_IsNumber(v)) c.bgEffect = v->valueint;
        if ((v = cJSON_GetObjectItem(doc, "MAT")) && cJSON_IsNumber(v)) c.matrix_layout = v->valueint;
        if ((v = cJSON_GetObjectItem(doc, "TCOL")) && cJSON_IsNumber(v)) c.textColor888 = (uint32_t)v->valuedouble;
        if ((v = cJSON_GetObjectItem(doc, "TIM")) && cJSON_IsBool(v)) c.showTime = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "DAT")) && cJSON_IsBool(v)) c.showDate = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "TEMP")) && cJSON_IsBool(v)) c.showTemp = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "HUM")) && cJSON_IsBool(v)) c.showHum = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "BAT")) && cJSON_IsBool(v)) c.showBat = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "SOUND")) && cJSON_IsBool(v)) c.soundActive = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "VOL")) && cJSON_IsNumber(v)) c.soundVolume = (uint8_t)v->valueint;
        if ((v = cJSON_GetObjectItem(doc, "DFP")) && cJSON_IsBool(v)) c.dfplayerActive = cJSON_IsTrue(v);
        /* Accept the original Arduino key "dfplayer" as well so existing
         * tooling keeps working (port restored in round 7 / pack E). */
        if ((v = cJSON_GetObjectItem(doc, "dfplayer")) && cJSON_IsBool(v)) c.dfplayerActive = cJSON_IsTrue(v);
        /* Display gamma + scroll speed + uppercase */
        if ((v = cJSON_GetObjectItem(doc, "GAM")) && cJSON_IsNumber(v)) c.gamma = (uint8_t)v->valueint;
        /* Pack I: original Arduino sent GAMMA as float (e.g. 1.9). Map it to
         * the new uint8_t-times-ten encoding so existing tooling keeps working. */
        if ((v = cJSON_GetObjectItem(doc, "GAMMA")) && cJSON_IsNumber(v)) c.gamma = (uint8_t)(
            v->valuedouble * 10.0 + 0.5);
        if ((v = cJSON_GetObjectItem(doc, "SSPEED")) && cJSON_IsNumber(v)) c.scroll_speed = v->valueint;
        if ((v = cJSON_GetObjectItem(doc, "UPPER")) && cJSON_IsBool(v)) c.uppercase_letters = cJSON_IsTrue(v);
        /* Pack I: legacy long key "UPPERCASE" — same semantics. */
        if ((v = cJSON_GetObjectItem(doc, "UPPERCASE")) && cJSON_IsBool(v)) c.uppercase_letters = cJSON_IsTrue(v);
        /* Pack I: MATP = matrix-power (true = on, so matrix_off = !MATP). */
        if ((v = cJSON_GetObjectItem(doc, "MATP")) && cJSON_IsBool(v)) c.matrix_off = !cJSON_IsTrue(v);
        /* Pack I: CCORRECTION / CTEMP — accept either hex string or [r,g,b]. */
        auto parse_rgb_into = [](cJSON* node, CRGB& out)
        {
            if (!node) return;
            if (cJSON_IsString(node) && node->valuestring)
            {
                uint32_t rgb = (uint32_t)strtoul(node->valuestring, nullptr, 16);
                out = CRGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
            }
            else if (cJSON_IsArray(node) && cJSON_GetArraySize(node) == 3)
            {
                cJSON* r = cJSON_GetArrayItem(node, 0);
                cJSON* g = cJSON_GetArrayItem(node, 1);
                cJSON* b = cJSON_GetArrayItem(node, 2);
                out = CRGB((uint8_t)(r ? r->valueint : 0),
                           (uint8_t)(g ? g->valueint : 0),
                           (uint8_t)(b ? b->valueint : 0));
            }
            else if (cJSON_IsNumber(node))
            {
                uint32_t rgb = (uint32_t)node->valuedouble;
                out = CRGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
            }
        };
        parse_rgb_into(cJSON_GetObjectItem(doc, "CCORRECTION"), c.colorCorrection);
        parse_rgb_into(cJSON_GetObjectItem(doc, "CTEMP"), c.colorTemperature);
        /* Pack I: helper for hex-or-number color into a uint32_t. */
        auto parse_u32_color = [](cJSON* node, uint32_t fallback) -> uint32_t
        {
            if (!node) return fallback;
            if (cJSON_IsNumber(node)) return (uint32_t)node->valuedouble;
            if (cJSON_IsString(node) && node->valuestring)
                return (uint32_t)strtoul(node->valuestring, nullptr, 16);
            return fallback;
        };
        if ((v = cJSON_GetObjectItem(doc, "CHCOL"))) c.calendarHeader = parse_u32_color(v, c.calendarHeader);
        if ((v = cJSON_GetObjectItem(doc, "CTCOL"))) c.calendarText = parse_u32_color(v, c.calendarText);
        if ((v = cJSON_GetObjectItem(doc, "CBCOL"))) c.calendarBody = parse_u32_color(v, c.calendarBody);
        /* Pack I: 5 per-app colors. Accept both the new condensed key
         * (TIMECOL/...) and the original underscored key (TIME_COL/...) so
         * existing tooling keeps working. */
        if ((v = cJSON_GetObjectItem(doc, "TIMECOL"))) c.timeColor = parse_u32_color(v, c.timeColor);
        if ((v = cJSON_GetObjectItem(doc, "DATECOL"))) c.dateColor = parse_u32_color(v, c.dateColor);
        if ((v = cJSON_GetObjectItem(doc, "TEMPCOL"))) c.tempColor = parse_u32_color(v, c.tempColor);
        if ((v = cJSON_GetObjectItem(doc, "HUMCOL"))) c.humColor = parse_u32_color(v, c.humColor);
        if ((v = cJSON_GetObjectItem(doc, "BATCOL"))) c.batColor = parse_u32_color(v, c.batColor);
        if ((v = cJSON_GetObjectItem(doc, "TIME_COL"))) c.timeColor = parse_u32_color(v, c.timeColor);
        if ((v = cJSON_GetObjectItem(doc, "DATE_COL"))) c.dateColor = parse_u32_color(v, c.dateColor);
        if ((v = cJSON_GetObjectItem(doc, "TEMP_COL"))) c.tempColor = parse_u32_color(v, c.tempColor);
        if ((v = cJSON_GetObjectItem(doc, "HUM_COL"))) c.humColor = parse_u32_color(v, c.humColor);
        if ((v = cJSON_GetObjectItem(doc, "BAT_COL"))) c.batColor = parse_u32_color(v, c.batColor);
        if ((v = cJSON_GetObjectItem(doc, "MIN_BRI")) && cJSON_IsNumber(v)) c.min_brightness = (uint8_t)v->valueint;
        if ((v = cJSON_GetObjectItem(doc, "MAX_BRI")) && cJSON_IsNumber(v)) c.max_brightness = (uint8_t)v->valueint;
        if ((v = cJSON_GetObjectItem(doc, "SB")) && cJSON_IsBool(v)) c.swap_buttons = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "ROT")) && cJSON_IsBool(v)) c.rotate_screen = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "MIR")) && cJSON_IsBool(v)) c.mirror_display = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "BLOCKN")) && cJSON_IsBool(v)) c.block_navigation = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "WD")) && cJSON_IsBool(v)) c.showWeekday = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "WDCA")) && cJSON_IsNumber(v)) c.wdcActive = (uint32_t)v->valuedouble;
        if ((v = cJSON_GetObjectItem(doc, "WDCI")) && cJSON_IsNumber(v)) c.wdcInactive = (uint32_t)v->valuedouble;
        if ((v = cJSON_GetObjectItem(doc, "TFORMAT")) && cJSON_IsString(v)) c.timeFormat = v->valuestring;
        if ((v = cJSON_GetObjectItem(doc, "DFORMAT")) && cJSON_IsString(v)) c.dateFormat = v->valuestring;
        if ((v = cJSON_GetObjectItem(doc, "TMODE")) && cJSON_IsNumber(v)) c.timeMode = v->valueint;
        if ((v = cJSON_GetObjectItem(doc, "SECONDS")) && cJSON_IsBool(v)) c.showSeconds = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "SOM")) && cJSON_IsBool(v)) c.startOnMonday = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "CEL")) && cJSON_IsBool(v)) c.isCelsius = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "TOFF")) && cJSON_IsNumber(v)) c.tempOffset = (float)v->valuedouble;
        if ((v = cJSON_GetObjectItem(doc, "HOFF")) && cJSON_IsNumber(v)) c.humOffset = (float)v->valuedouble;
        if ((v = cJSON_GetObjectItem(doc, "FPS")) && cJSON_IsNumber(v)) c.matrix_fps = v->valueint;
        if ((v = cJSON_GetObjectItem(doc, "WEA")) && cJSON_IsBool(v)) c.showWeather = cJSON_IsTrue(v);
        /* Global weather overlay: accept string ("rain"/"snow"/...) or numeric enum. */
        if ((v = cJSON_GetObjectItem(doc, "OVERLAY")))
        {
            if (cJSON_IsNumber(v)) c.globalOverlay = (OverlayEffect)v->valueint;
            else if (cJSON_IsString(v)) c.globalOverlay = parseOverlayName(v->valuestring);
        }
        /* Networking */
        if ((v = cJSON_GetObjectItem(doc, "NTPSV")) && cJSON_IsString(v)) c.ntp_server = v->valuestring;
        if ((v = cJSON_GetObjectItem(doc, "TZ")) && cJSON_IsString(v)) c.ntp_tz = v->valuestring;
        if ((v = cJSON_GetObjectItem(doc, "MQUSER")) && cJSON_IsString(v)) c.mqtt_user = v->valuestring;
        if ((v = cJSON_GetObjectItem(doc, "MQPASS")) && cJSON_IsString(v)) c.mqtt_pass = v->valuestring;
        if ((v = cJSON_GetObjectItem(doc, "MQHOST")) && cJSON_IsString(v)) c.mqtt_host = v->valuestring;
        if ((v = cJSON_GetObjectItem(doc, "MQPORT")) && cJSON_IsNumber(v)) c.mqtt_port = (uint16_t)v->valueint;
        if ((v = cJSON_GetObjectItem(doc, "MQPRE")) && cJSON_IsString(v)) c.mqtt_prefix = v->valuestring;
        if ((v = cJSON_GetObjectItem(doc, "HAP")) && cJSON_IsBool(v)) c.ha_discovery = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(doc, "AUTHU")) && cJSON_IsString(v)) c.auth_user = v->valuestring;
        if ((v = cJSON_GetObjectItem(doc, "AUTHP")) && cJSON_IsString(v)) c.auth_pass = v->valuestring;
        if ((v = cJSON_GetObjectItem(doc, "BTNCB")) && cJSON_IsString(v)) c.buttonCallback = v->valuestring;
        cJSON_Delete(doc);
        c.save();
    }
    /* applyAllSettings() only touches m_ui/m_matrix (no shared containers).
     * loadNativeApps() acquires its own m_dataLock — must be called AFTER we
     * release ours to avoid a recursive (deadlock-prone) portMUX entry. */
    applyAllSettings();
    loadNativeApps();
}

void DisplayManager::setMatrixLayout(int layout)
{
    int brightness = 0;
    {
        AwtrixConfig::Guard cfgGuard(CONFIG);
        CONFIG.matrix_layout = layout;
        brightness = CONFIG.brightness;
    }
    if (m_matrix) m_matrix->setBrightness(brightness);
}

/* ── gamma correction: re-apply to all pixels via Matrix passthrough.
 * The original used FastLED's gamma8 table; we use simple x^gamma. */
void DisplayManager::gammaCorrection()
{
    if (!m_matrix || CONFIG.gamma <= 0) return;
    int total = m_matrix->width() * m_matrix->height();
    CRGB* leds = m_matrix->getLeds();
    float g = (float)CONFIG.gamma / 100.0f;
    if (g < 0.1f) g = 1.0f;
    for (int i = 0; i < total; i++)
    {
        leds[i].r = (uint8_t)(powf(leds[i].r / 255.0f, g) * 255.0f);
        leds[i].g = (uint8_t)(powf(leds[i].g / 255.0f, g) * 255.0f);
        leds[i].b = (uint8_t)(powf(leds[i].b / 255.0f, g) * 255.0f);
    }
}

void DisplayManager::setCustomAppColors(uint32_t color)
{
    for (auto& kv : APP_REGISTRY.customApps())
    {
        kv.second.color = color;
        kv.second.hasCustomColor = true;
    }
}

/* ── visual sleep animation: brief fade-to-black */
void DisplayManager::showSleepAnimation()
{
    if (!m_matrix) return;
    RENDER_ENGINE.setMode(AwtrixDisplayMode::Sleep);
    for (int b = CONFIG.brightness; b >= 0; b -= 10)
    {
        m_matrix->setBrightness(b);
        m_matrix->show();
        update_display_snapshot_from_matrix(m_matrix);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    m_matrix->clear();
    m_matrix->show();
    update_display_snapshot_from_matrix(m_matrix);
}

void DisplayManager::sendAppLoop()
{
    /* Mirrors original: re-publishes the currently active app name + state
     * via the MQTT manager. The MQTT publish path lives in
     * components/awtrix_network (declared weak). */
    extern void awtrix_mqtt_publish_app_loop(const char*) __attribute__
    ((weak));
    if (m_ui && &awtrix_mqtt_publish_app_loop)
    {
        std::string name;
        {
            Lock _l(&m_dataLock);
            UiState* st = m_ui->getUiState();
            name = st ? APP_REGISTRY.appNameAt(st->currentApp) : "";
        }
        awtrix_mqtt_publish_app_loop(name.c_str());
    }
}

void DisplayManager::checkNewYear()
{
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    /* Last 10 seconds of Dec 31 → trigger banner via notification */
    if (t && t->tm_mon == 11 && t->tm_mday == 31 && t->tm_hour == 23 && t->tm_min == 59 && t->tm_sec >= 50)
    {
        const char* json = "{\"text\":\"HAPPY NEW YEAR\",\"duration\":15,\"color\":\"FF8800\"}";
        generateNotification(0, json);
        {
            AwtrixConfig::Guard cfgGuard(CONFIG);
            CONFIG.newYear = true;
        }
    }
}

/* Art-Net is an optional DMX-over-Wi-Fi feature. Wired via weak symbol so
 * the awtrix_artnet sub-module can register itself without a hard dep. */
void DisplayManager::startArtnet()
{
    extern void awtrix_artnet_start() __attribute__
    ((weak));
    if (&awtrix_artnet_start) awtrix_artnet_start();
}

/* ── Indicator color/state shortcuts (forward to UI). ─────────
 * M3 (round 8): six near-identical forwarders compressed via a
 * single local macro. To add Indicator4 etc., add one more
 * AWTRIX_INDICATOR_FORWARD(4) row. */
#define AWTRIX_INDICATOR_FORWARD(N)                                                            \
    void DisplayManager::setIndicator##N##Color(uint32_t c) { if (m_ui) m_ui->setIndicator##N##Color(c); } \
    void DisplayManager::setIndicator##N##State(bool on)    { if (m_ui) m_ui->setIndicator##N##State(on);  }

AWTRIX_INDICATOR_FORWARD(1)
AWTRIX_INDICATOR_FORWARD(2)
AWTRIX_INDICATOR_FORWARD(3)

#undef AWTRIX_INDICATOR_FORWARD

/* ── Effect / transition / icon catalogs (JSON strings) ────── */
std::string DisplayManager::getEffectNames() const
{
    return std::string(::getEffectNames());
}

std::string DisplayManager::getTransitionNames() const
{
    static const char* names[] = {
        "Random", "Slide", "Fade", "Zoom", "Rotate", "Pixelate",
        "Curtain", "Ripple", "Blink", "Reload", "Crossfade"
    };
    std::string s = "[";
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (i) s += ',';
        s += '"';
        s += names[i];
        s += '"';
    }
    s += ']';
    return s;
}

extern "C" const char* awtrix_core_query_apps_json(void)
{
    static thread_local std::string value;
    value = DisplayManager::get().getAppsAsJson();
    return value.c_str();
}

extern "C" const char* awtrix_core_query_settings_json(void)
{
    static thread_local std::string value;
    value = DisplayManager::get().getSettings();
    return value.c_str();
}

extern "C" const char* awtrix_core_query_stats_json(void)
{
    static thread_local std::string value;
    value = DisplayManager::get().getStats();
    return value.c_str();
}

extern "C" const char* awtrix_core_query_apps_with_icon_json(void)
{
    static thread_local std::string value;
    value = DisplayManager::get().getAppsWithIcon();
    return value.c_str();
}

extern "C" const char* awtrix_core_query_effect_names_json(void)
{
    static thread_local std::string value;
    value = DisplayManager::get().getEffectNames();
    return value.c_str();
}

extern "C" const char* awtrix_core_query_transition_names_json(void)
{
    static thread_local std::string value;
    value = DisplayManager::get().getTransitionNames();
    return value.c_str();
}

std::string DisplayManager::getAppsWithIcon() const
{
    Lock _l(&m_dataLock);
    cJSON* root = cJSON_CreateArray();
    for (auto& a : APP_REGISTRY.apps())
    {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", a.first.c_str());
        const CustomApp* customApp = APP_REGISTRY.findCustomApp(a.first);
        if (customApp)
        {
            cJSON_AddStringToObject(o, "icon", customApp->iconName.c_str());
        }
        else
        {
            cJSON_AddStringToObject(o, "icon", "");
        }
        cJSON_AddItemToArray(root, o);
    }
    char* s = cJSON_PrintUnformatted(root);
    std::string r(s ? s : "[]");
    if (s) cJSON_free(s);
    cJSON_Delete(root);
    return r;
}

/* ── Weather app: renders cached values from CONFIG.weather* ─ */
void WeatherApp(Matrix& m, UiState&, int16_t x, int16_t y, GifPlayer*)
{
    auto& c = CONFIG;
    char buf[24];
    snprintf(buf, sizeof(buf), "%.0f°C", c.currentTemp + c.tempOffset);
    m.setCursor(x + 2, y + 1);
    m.setTextColor(c.timeColor ? c.timeColor : c.textColor888);
    m.print(buf);
}