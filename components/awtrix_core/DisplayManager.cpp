#include "DisplayManager.h"
#include "awtrix_globals.h"
#include "effects_core.h"
#include "awtrix_events.h"     /* EVENTS.rtttl(...) — replaces direct awtrix_io.h dep */
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

static uint64_t _now() { return esp_timer_get_time() / 1000; }

/* ── Forward declarations / file-scope shared state ─────────────
 * These need to be visible to tick() / updateAppVector() which appear
 * before their definitions later in the file. */
static void renderCustomApp(Matrix& m, UiState& state, int16_t x, int16_t y, GifPlayer*);
static uint32_t s_moodlightColor = 0x000000;
static bool s_moodlightActive = false;

/* ── Built-in app callbacks ─────────────────────────────────── */
void TimeApp(Matrix& m, UiState&, int16_t x, int16_t y, GifPlayer*)
{
    auto& c = CONFIG;
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", ti->tm_hour, ti->tm_min);
    uint32_t col = c.timeColor ? c.timeColor : c.textColor888;
    m.setCursor(x + 2, y + 1);
    m.setTextColor(col);
    m.print(buf);
}

void DateApp(Matrix& m, UiState&, int16_t x, int16_t y, GifPlayer*)
{
    auto& c = CONFIG;
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d.%02d", ti->tm_mday, ti->tm_mon + 1);
    uint32_t col = c.dateColor ? c.dateColor : c.textColor888;
    m.setCursor(x + 2, y + 1);
    m.setTextColor(col);
    m.print(buf);
}

void TempApp(Matrix& m, UiState&, int16_t x, int16_t y, GifPlayer*)
{
    auto& c = CONFIG;
    char buf[16];
    float temp = c.currentTemp + c.tempOffset;
    snprintf(buf, sizeof(buf), "%.*f°C", c.tempDecimalPlaces, temp);
    uint32_t col = c.tempColor ? c.tempColor : c.textColor888;
    m.setCursor(x + 2, y + 1);
    m.setTextColor(col);
    m.print(buf);
}

void HumApp(Matrix& m, UiState&, int16_t x, int16_t y, GifPlayer*)
{
    auto& c = CONFIG;
    char buf[12];
    snprintf(buf, sizeof(buf), "%.0f%%", c.currentHum);
    uint32_t col = c.humColor ? c.humColor : c.textColor888;
    m.setCursor(x + 2, y + 1);
    m.setTextColor(col);
    m.print(buf);
}

void BatApp(Matrix& m, UiState&, int16_t x, int16_t y, GifPlayer*)
{
    auto& c = CONFIG;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d%%", c.batteryPercent);
    uint32_t col = c.batColor ? c.batColor : c.textColor888;
    m.setCursor(x + 2, y + 1);
    m.setTextColor(col);
    m.print(buf);
}

/* ── Notification overlay ───────────────────────────────────────
 * Mirrors src/Overlays.cpp::NotifyOverlay from the original AWTRIX3.
 * Runs after the active app every frame: if there is a queued
 * notification, overlay it on top of the matrix until duration / repeat
 * exhausts. Then evict it from the queue so the next one shows.
 */
static OverlayCallback s_notifyOverlay = [](Matrix& m, UiState&, GifPlayer*)
{
    auto& dm = DisplayManager::get();
    auto& q = dm.m_notifications;
    if (q.empty()) return;
    AwtrixNotification& n = q.front();

    /* Initialize scroll position the first frame this notification is shown */
    if (n.startTime == 0)
    {
        n.startTime = (long)(esp_timer_get_time() / 1000);
        n.scrollPosition = (float)m.width(); /* enter from the right */
    }

    long now = (long)(esp_timer_get_time() / 1000);
    long elapsedMs = now - n.startTime;

    /* Background wash */
    if (n.bgColor)
    {
        m.fillRect(0, 0, m.width(), m.height(), n.bgColor);
    }
    else
    {
        m.fillRect(0, 0, m.width(), m.height(), 0x000000);
    }

    /* Optional FX as a backdrop layer (effect indices match effects_core.h) */
    if (n.effect >= 0) callEffect(m, 0, 0, n.effect);

    /* Global weather overlay (rain/snow/storm/thunder/frost/drizzle) — drawn
     * ON TOP of the FX backdrop but UNDER the notification text, mirroring
     * the original Overlays.cpp behaviour. */
    if (CONFIG.globalOverlay > OVERLAY_TIME)
        EffectOverlay(m, 0, 0, (int)CONFIG.globalOverlay);

    /* Lazily kick off RTTTL once per notification */
    if (n.rtttl && !n.soundPlayed)
    {
        /* Notification carries the raw RTTTL string in `sound` (set by parser).
         * Dispatch through the event bus so the periphery binding stays
         * decoupled — this removes the awtrix_core → awtrix_periphery
         * direct symbol reference. */
        if (!n.sound.empty()) EVENTS.rtttl(n.sound.c_str());
        n.soundPlayed = true;
    }

    /* Text rendering: noScrolling=true → centered, else slide right→left */
    uint32_t col = n.color ? n.color : CONFIG.textColor888;
    m.setTextColor(col);
    int textW = (int)n.text.size() * 4; /* 3px glyph + 1px gap, approx */
    if (textW <= m.width() || n.noScrolling)
    {
        int tx = (m.width() - textW) / 2;
        if (tx < 0) tx = 0;
        m.setCursor(tx, 1);
        m.print(n.text.c_str());
    }
    else
    {
        m.setCursor((int)n.scrollPosition, 1);
        m.print(n.text.c_str());
        n.scrollPosition -= 0.5f; /* ~0.5 px per frame ≈ 30 px/s */
        if (n.scrollPosition < -textW)
        {
            n.scrollPosition = (float)m.width();
            if (n.repeat > 0)
            {
                n.repeat--;
                if (n.repeat == 0)
                {
                    /* exhausted scrolls */
                    q.erase(q.begin());
                    return;
                }
            }
        }
    }

    /* Duration-based eviction (duration is in seconds). When repeat != -1
     * the eviction is driven by the scroll loop above instead. */
    if (n.repeat == -1 && n.duration > 0 && elapsedMs >= (long)n.duration * 1000L)
    {
        q.erase(q.begin());
    }
};

/* Static storage no longer required: MatrixDisplayUi::setOverlays takes a
 * const std::vector<OverlayCallback>& and copies the entries internally. */

/* ── Status overlay: tiny WiFi/AP/MQTT dot at the top-right ────
 * Mirrors original src/Overlays.cpp::StatusOverlay. Drawn after each app
 * frame so the WiFi/AP indicator never gets clobbered by app rendering. */
extern "C" bool awtrix_wifi_is_ready(void);
extern "C" bool awtrix_wifi_is_connected(void);

static OverlayCallback s_statusOverlay = [](Matrix& m, UiState&, GifPlayer*)
{
    if (!awtrix_wifi_is_ready()) return;
    bool sta = awtrix_wifi_is_connected();
    m.drawPixel(m.width() - 1, 0, sta ? 0x00FF00 : 0xFFA000);
};

/* ── Setup ───────────────────────────────────────────────────── */
void DisplayManager::setup(Matrix* m)
{
    m_matrix = m;
    m_ui = new MatrixDisplayUi(m);
    m_ui->init();
    std::vector<OverlayCallback> overlays = {s_notifyOverlay, s_statusOverlay};
    m_ui->setOverlays(overlays);
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

/* ── Tick ────────────────────────────────────────────────────── */
void DisplayManager::tick()
{
    if (!m_ui) return;
    if (m_gameActive) return;
    if (m_matrixOff) return;

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
        m_matrix->fillRect(0, 0, m_matrix->width(), m_matrix->height(), moodColor);
        if (CONFIG.gamma > 0) gammaCorrection();
        m_matrix->show();
        return;
    }

    m_ui->update();

    /* Apply gamma correction to the framebuffer (when configured) before the
     * next show(), mirroring the original FastLED gamma8 pass. */
    if (CONFIG.gamma > 0) gammaCorrection();

    if (m_ui->getUiState()->appState == UiState::IN_TRANSITION && !m_appIsSwitching)
    {
        m_appIsSwitching = true;
    }
    else if (m_ui->getUiState()->appState == UiState::FIXED && m_appIsSwitching)
    {
        m_appIsSwitching = false;
    }
}

/* ── App management ──────────────────────────────────────────── */
/* Internal, lock-free implementation. Caller MUST hold m_dataLock. */
static void loadNativeApps_impl(std::vector<std::pair<std::string, AppCallback>>& apps,
                                MatrixDisplayUi* ui)
{
    auto& c = CONFIG;
    apps.clear();

    if (c.showTime) apps.push_back({"Time", TimeApp});
    if (c.showDate) apps.push_back({"Date", DateApp});
    if (c.showTemp) apps.push_back({"Temp", TempApp});
    if (c.showHum) apps.push_back({"Hum", HumApp});
    if (c.showBat) apps.push_back({"Bat", BatApp});
    if (c.showWeather) apps.push_back({"Weather", WeatherApp});

    if (ui) ui->setApps(apps);
}

void DisplayManager::loadNativeApps()
{
    Lock _l(&m_dataLock);
    loadNativeApps_impl(m_apps, m_ui);
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
    for (size_t i = 0; i < m_apps.size(); i++)
    {
        if (m_apps[i].first == n)
        {
            m_ui->transitionToApp(i);
            return true;
        }
    }
    return false;
}

void DisplayManager::updateAppVector(const char* json)
{
    /* parse JSON with "apps": ["Time","Date"...] and update m_apps */
    cJSON* doc = cJSON_Parse(json);
    if (!doc) return;
    cJSON* arr = cJSON_GetObjectItem(doc, "apps");
    if (!arr || !cJSON_IsArray(arr))
    {
        cJSON_Delete(doc);
        return;
    }

    Lock _l(&m_dataLock);
    m_apps.clear();
    for (int i = 0; i < cJSON_GetArraySize(arr); i++)
    {
        cJSON* item = cJSON_GetArrayItem(arr, i);
        if (item && item->valuestring)
        {
            std::string n(item->valuestring);
            if (n == "Time") m_apps.push_back({"Time", TimeApp});
            else if (n == "Date") m_apps.push_back({"Date", DateApp});
            else if (n == "Temp") m_apps.push_back({"Temp", TempApp});
            else if (n == "Hum") m_apps.push_back({"Hum", HumApp});
            else if (n == "Bat") m_apps.push_back({"Bat", BatApp});
            else if (n == "Weather") m_apps.push_back({"Weather", WeatherApp});
            else if (m_customApps.find(n) != m_customApps.end())
            {
                /* Re-link an existing custom app to the shared renderer. */
                m_apps.push_back({n, renderCustomApp});
            }
            /* else: unknown name → silently skip rather than push nullptr. */
        }
    }
    m_ui->setApps(m_apps);
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

    /* Defer startTime: NotifyOverlay sets it on the first render frame so the
     * `duration` clock starts when the user actually sees the notification. */
    notif.startTime = 0;

    {
        Lock _l(&m_dataLock);
        if (notif.stack || m_notifications.empty())
        {
            m_notifications.push_back(notif);
        }
        else
        {
            m_notifications[0] = notif; /* replace the head, mirrors original */
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
    if (!m_notifications.empty())
        m_notifications.erase(m_notifications.begin());
}

/* ── Drawing ─────────────────────────────────────────────────── */
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
    cJSON* root = cJSON_CreateArray();
    for (auto& a : m_apps) cJSON_AddItemToArray(root, cJSON_CreateString(a.first.c_str()));
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
    cJSON* r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "uptime", _now() / 1000);
    cJSON_AddNumberToObject(r, "received_messages", CONFIG.receivedMessages);
    cJSON_AddStringToObject(r, "version", CONFIG.version);
    char* s = cJSON_PrintUnformatted(r);
    std::string res(s);
    cJSON_free(s);
    cJSON_Delete(r);
    return res;
}

std::string DisplayManager::ledsAsJson() const
{
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
    std::string res(s);
    cJSON_free(s);
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
            s_moodlightActive = false;
            CONFIG.moodlightMode = false;
        }
        ESP_LOGI(TAG, "Moodlight: off");
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
            s_moodlightActive = false;
            CONFIG.moodlightMode = false;
        }
        ESP_LOGI(TAG, "Moodlight: off");
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
        s_moodlightColor = rgb;
        s_moodlightActive = true;
        CONFIG.moodlightMode = true;
    }
    ESP_LOGI(TAG, "Moodlight: 0x%06X", (unsigned)(rgb & 0xFFFFFF));

    /* Paint immediately so the user sees the change without waiting for tick(). */
    if (m_matrix)
    {
        m_matrix->fillRect(0, 0, m_matrix->width(), m_matrix->height(), rgb);
        m_matrix->show();
    }
    return true;
}

/* ── Custom app rendering ───────────────────────────────────────
 * Minimal viable implementation. Each custom app stores a text + color
 * in DisplayManager::m_customApps (keyed by app name). Every custom
 * slot in m_apps shares the same renderer lambda which looks the data
 * back up by name from the UiState::userData pointer.
 *
 * This deliberately omits icons, GIFs, draw instructions, fragments,
 * bar/line charts and progress bars from the original Apps.cpp; those
 * are large subsystems and are not required for "minimal viable"
 * /api/custom support requested by the user.
 */
static void renderCustomApp(Matrix& m, UiState& state, int16_t x, int16_t y, GifPlayer*)
{
    auto& dm = DisplayManager::get();

    /* Resolve (slot index → app name → CustomApp) under the data lock so
     * a concurrent /api/custom or MQTT custom/+ message can't invalidate
     * the m_apps vector or m_customApps map mid-read. We copy the small
     * CustomApp struct to a local so the actual render runs lock-free. */
    CustomApp app;
    {
        DisplayManager::Lock _l(&dm.m_dataLock);
        const auto& apps = dm.peekApps();
        if (state.currentApp >= apps.size()) return;
        const std::string& slotName = apps[state.currentApp].first;
        auto it = dm.peekCustomApps().find(slotName);
        if (it == dm.peekCustomApps().end()) return;
        app = it->second; /* deep copy (~few hundred bytes) */
    }

    /* (0) blink: skip rendering for half of every (2*blink) ms cycle. */
    if (app.blink > 0)
    {
        uint64_t t = esp_timer_get_time() / 1000;
        if ((t / app.blink) & 1) return;
    }

    if (app.background)
    {
        m.fillRect(x, y, m.width(), m.height(), app.background);
    }
    if (app.effect >= 0) callEffect(m, x, y, app.effect);

    /* Global weather overlay (drizzle/rain/snow/storm/thunder/frost) is
     * applied on top of the per-app FX backdrop, mirroring the original
     * AWTRIX3 behaviour where every frame renders the weather overlay. */
    if (CONFIG.globalOverlay > OVERLAY_TIME)
        EffectOverlay(m, 0, 0, (int)CONFIG.globalOverlay);

    uint32_t col = app.hasCustomColor ? app.color : CONFIG.textColor888;
    m.setTextColor(col);

    /* (1) Bar chart: vertical bars from the bottom, max height = matrix height. */
    if (app.barSize > 0)
    {
        int W = m.width();
        int H = m.height();
        int slot = app.barSize > 0 ? W / app.barSize : W;
        if (slot < 1) slot = 1;
        for (int i = 0; i < app.barSize; i++)
        {
            int v = app.barData[i];
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            int h = (v * H) / 100;
            int bx = x + i * slot;
            if (app.barBG) m.fillRect(bx, y, slot - 1, H, app.barBG);
            m.fillRect(bx, y + (H - h), slot - 1, h, col);
        }
    }
    /* (2) Line chart: connect successive points with drawLine. */
    if (app.lineSize > 1)
    {
        int W = m.width();
        int H = m.height();
        int step = (W - 1) / (app.lineSize - 1);
        for (int i = 1; i < app.lineSize; i++)
        {
            int v0 = app.lineData[i - 1];
            int v1 = app.lineData[i];
            if (v0 < 0) v0 = 0;
            if (v0 > 100) v0 = 100;
            if (v1 < 0) v1 = 0;
            if (v1 > 100) v1 = 100;
            int y0 = y + (H - 1) - (v0 * (H - 1)) / 100;
            int y1 = y + (H - 1) - (v1 * (H - 1)) / 100;
            m.drawLine(x + (i - 1) * step, y0, x + i * step, y1, col);
        }
    }

    /* (3) drawInstructions DSL (pixel/line/rect/circle/text/palette ops).
     * Consume the POD cache compiled in parseCustomPage instead of
     * re-parsing the JSON every frame. The cache lives on the local
     * `app` copy taken under the lock above, so this stays race-free. */
    if (!app.compiledDraw.empty())
    {
        DisplayManager::get().processCompiledDraw(x, y, app.compiledDraw, app.drawTexts);
    }

    /* (4) Progress bar: 1px tall track at bottom row + filled portion. */
    if (app.progress >= 0 && app.progress <= 100)
    {
        int W = m.width();
        int filled = (app.progress * W) / 100;
        int by = y + m.height() - 1;
        if (app.pbColor) m.fillRect(x, by, W, 1, app.pbColor);
        if (filled > 0) m.fillRect(x, by, filled, 1, app.pColor ? app.pColor : col);
    }

    /* (5) Multi-color fragments: print each fragment with its own color
     * advancing the cursor by ~4 px per glyph. */
    if (!app.fragments.empty())
    {
        int cx = x;
        int cy = y + 1;
        if (app.center)
        {
            int totalW = 0;
            for (auto& s : app.fragments) totalW += (int)s.size() * 4;
            cx = x + (m.width() - totalW) / 2;
            if (cx < x) cx = x;
        }
        for (size_t i = 0; i < app.fragments.size(); i++)
        {
            uint32_t fc = (i < app.colors.size()) ? app.colors[i] : col;
            m.setTextColor(fc);
            m.setCursor(cx, cy + app.textOffset);
            m.print(app.fragments[i].c_str());
            cx += (int)app.fragments[i].size() * 4;
        }
        return;
    }

    /* (6) Apply textCase: 1=upper, 2=lower. Build a temp buffer if needed. */
    const char* txt = app.text.c_str();
    char tmp[256];
    if (app.textCase == 1 || app.textCase == 2)
    {
        size_t n = app.text.size();
        if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
        for (size_t i = 0; i < n; i++)
        {
            char c = app.text[i];
            tmp[i] = app.textCase == 1
                         ? (char)toupper((unsigned char)c)
                         : (char)tolower((unsigned char)c);
        }
        tmp[n] = 0;
        txt = tmp;
    }

    /* Default text path: centered or left-aligned (top text optional). */
    int textW = (int)strlen(txt) * 4;
    int tx = x;
    if (app.center || textW <= m.width())
        tx = x + ((m.width() - textW) / 2);
    if (tx < x) tx = x;
    int ty = y + 1 + app.textOffset;
    if (!app.topText) ty = y + m.height() - 7 + app.textOffset;
    m.setCursor(tx, ty);
    m.print(txt);
}

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
            m_customApps.erase(sname);
            for (auto it = m_apps.begin(); it != m_apps.end();)
            {
                if (it->first == sname) it = m_apps.erase(it);
                else ++it;
            }
            if (m_ui) m_ui->setApps(m_apps);
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

    /* Atomic swap-in + m_apps insertion under the data lock. */
    {
        Lock _l(&m_dataLock);
        m_customApps[sname] = std::move(app);

        auto eIt = std::find_if(m_apps.begin(), m_apps.end(),
                                [&](const std::pair<std::string, AppCallback>& p) { return p.first == sname; });
        if (eIt == m_apps.end())
        {
            m_apps.push_back({sname, renderCustomApp});
        }
        else
        {
            eIt->second = renderCustomApp;
        }
        if (m_ui) m_ui->setApps(m_apps);
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

    ESP_LOGI(TAG, "Custom app '%s' upserted (text=\"%s\")", name, app.text.c_str());
    return true;
}

/* Read-only accessor used by renderCustomApp() above. */
const std::map<std::string, CustomApp>& DisplayManager::peekCustomApps() const
{
    return m_customApps;
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
void DisplayManager::show() { if (m_matrix) m_matrix->show(); }

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
    /* All CONFIG.* writes happen inside this critical section so a concurrent
     * tick() / render never observes a half-applied settings batch. */
    {
        Lock _l(&m_dataLock);
        auto& c = CONFIG;
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
        /* Display gamma + scroll speed + uppercase */
        if ((v = cJSON_GetObjectItem(doc, "GAM")) && cJSON_IsNumber(v)) c.gamma = (uint8_t)v->valueint;
        if ((v = cJSON_GetObjectItem(doc, "SSPEED")) && cJSON_IsNumber(v)) c.scroll_speed = v->valueint;
        if ((v = cJSON_GetObjectItem(doc, "UPPER")) && cJSON_IsBool(v)) c.uppercase_letters = cJSON_IsTrue(v);
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
    CONFIG.matrix_layout = layout;
    if (m_matrix) m_matrix->setBrightness(CONFIG.brightness);
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
    for (auto& kv : m_customApps)
    {
        kv.second.color = color;
        kv.second.hasCustomColor = true;
    }
}

/* ── visual sleep animation: brief fade-to-black */
void DisplayManager::showSleepAnimation()
{
    if (!m_matrix) return;
    for (int b = CONFIG.brightness; b >= 0; b -= 10)
    {
        m_matrix->setBrightness(b);
        m_matrix->show();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    m_matrix->clear();
    m_matrix->show();
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
        UiState* st = m_ui->getUiState();
        const char* name = (st && st->currentApp < m_apps.size())
                               ? m_apps[st->currentApp].first.c_str()
                               : "";
        awtrix_mqtt_publish_app_loop(name);
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
        CONFIG.newYear = true;
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

/* ── Indicator color/state shortcuts (forward to UI). ───────── */
void DisplayManager::setIndicator1Color(uint32_t c) { if (m_ui) m_ui->setIndicator1Color(c); }
void DisplayManager::setIndicator1State(bool on) { if (m_ui) m_ui->setIndicator1State(on); }
void DisplayManager::setIndicator2Color(uint32_t c) { if (m_ui) m_ui->setIndicator2Color(c); }
void DisplayManager::setIndicator2State(bool on) { if (m_ui) m_ui->setIndicator2State(on); }
void DisplayManager::setIndicator3Color(uint32_t c) { if (m_ui) m_ui->setIndicator3Color(c); }
void DisplayManager::setIndicator3State(bool on) { if (m_ui) m_ui->setIndicator3State(on); }

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

std::string DisplayManager::getAppsWithIcon() const
{
    cJSON* root = cJSON_CreateArray();
    for (auto& a : m_apps)
    {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", a.first.c_str());
        auto it = m_customApps.find(a.first);
        if (it != m_customApps.end())
        {
            cJSON_AddStringToObject(o, "icon", it->second.iconName.c_str());
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