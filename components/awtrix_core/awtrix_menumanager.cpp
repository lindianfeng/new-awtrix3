/**
 * Pack D (round 7): full re-port of src/MenuManager.cpp. The original
 * supported 13 menu items reachable via SELECT-long-press; this port
 * matches that item list and wires the renderer into the display
 * overlay chain so the matrix actually shows the current selection
 * while the user is navigating.
 *
 * Items (in selection order, mirroring the Arduino original):
 *   BRIGHT / COLOR / SWITCH / T-SPEED / APPTIME / TIME / DATE / WEEKDAY
 *   / TEMP / APPS / SOUND / VOLUME / SwBtn (replaces removed UPDATE)
 */
#include "awtrix_menumanager.h"
#include "awtrix_globals.h"
#include "DisplayManager.h"
#include "esp_log.h"
#include <cstring>

#define TAG "menu"

namespace {

struct MenuItem {
    const char *label;
    void (*onLeft) ();
    void (*onRight)();
    const char *(*display)();
};

static char s_buf[24];

static const char *fmt_int(int v)  { snprintf(s_buf, sizeof(s_buf), "%d",  v); return s_buf; }
static const char *fmt_bool(bool v){ return v ? "ON" : "OFF"; }

/* ── Format presets ─────────────────────────────────────────── */
static const char *kTimeFormats[] = {
    "%H:%M:%S", "%l:%M:%S", "%H:%M", "%H %M",
    "%l:%M",    "%l %M",    "%l:%M %p", "%l %M %p",
};
static const int kTimeFormatCount = (int)(sizeof(kTimeFormats) / sizeof(kTimeFormats[0]));

static const char *kDateFormats[] = {
    "%d.%m.%y", "%d.%m.",   "%y-%m-%d", "%m-%d",
    "%m/%d/%y", "%m/%d",    "%d/%m/%y", "%d/%m",
    "%m-%d-%y",
};
static const int kDateFormatCount = (int)(sizeof(kDateFormats) / sizeof(kDateFormats[0]));

/* COLOR menu cycles through this palette (matches the original
 * MenuManager textColors[] order). */
static const uint32_t kTextColors[] = {
    0xFFFFFF, 0xFFC000, 0xFFFF00, 0x00FF00, 0x00FFFF,
    0x0000FF, 0xFF00FF, 0xFF0000, 0xFF8800, 0x8800FF,
    0xFF80FF, 0x40FF40, 0xFFCC80, 0x80FFFF, 0xFF4080,
};
static const int kTextColorCount = (int)(sizeof(kTextColors) / sizeof(kTextColors[0]));

static int s_colorIdx = 0;
static int s_timeFmtIdx = 0;
static int s_dateFmtIdx = 0;
static int s_appsIdx = 0;          /* 0=Time 1=Date 2=Temp 3=Hum 4=Bat */

/* ── BRIGHT ─────────────────────────────────────────────────── */
static void inc_bri()   { AwtrixConfig::Guard g(CONFIG); CONFIG.brightness = (CONFIG.brightness < 250) ? CONFIG.brightness + 5 : 255; }
static void dec_bri()   { AwtrixConfig::Guard g(CONFIG); CONFIG.brightness = (CONFIG.brightness >   5) ? CONFIG.brightness - 5 : 0;   }
static const char *show_bri() { return fmt_int(CONFIG.brightness); }

/* ── COLOR ──────────────────────────────────────────────────── */
static void cycle_color_right() {
    AwtrixConfig::Guard g(CONFIG);
    s_colorIdx = (s_colorIdx + 1) % kTextColorCount;
    CONFIG.textColor888 = kTextColors[s_colorIdx];
}
static void cycle_color_left() {
    AwtrixConfig::Guard g(CONFIG);
    s_colorIdx = (s_colorIdx + kTextColorCount - 1) % kTextColorCount;
    CONFIG.textColor888 = kTextColors[s_colorIdx];
}
static const char *show_color() {
    snprintf(s_buf, sizeof(s_buf), "0x%06lX", (unsigned long)CONFIG.textColor888 & 0xFFFFFFu);
    return s_buf;
}

/* ── SWITCH (auto_transition) ───────────────────────────────── */
static void tog_atrans() { AwtrixConfig::Guard g(CONFIG); CONFIG.auto_transition = !CONFIG.auto_transition; }
static const char *show_atrans() { return fmt_bool(CONFIG.auto_transition); }

/* ── T-SPEED (transition speed in ms) ───────────────────────── */
static void inc_tspeed() { AwtrixConfig::Guard g(CONFIG); CONFIG.timePerTransition = (CONFIG.timePerTransition < 2000) ? CONFIG.timePerTransition + 100 : 2000; }
static void dec_tspeed() { AwtrixConfig::Guard g(CONFIG); CONFIG.timePerTransition = (CONFIG.timePerTransition >  200) ? CONFIG.timePerTransition - 100 : 100;  }
static const char *show_tspeed() { return fmt_int((int)CONFIG.timePerTransition); }

/* ── APPTIME (per-app dwell in s) ───────────────────────────── */
static void inc_atime()   { AwtrixConfig::Guard g(CONFIG); CONFIG.timePerApp += 500; }
static void dec_atime()   { AwtrixConfig::Guard g(CONFIG); if (CONFIG.timePerApp > 1000) CONFIG.timePerApp -= 500; }
static const char *show_atime() { return fmt_int((int)(CONFIG.timePerApp / 1000)); }

/* ── TIME (format preset) ───────────────────────────────────── */
static void cycle_time_right() {
    AwtrixConfig::Guard g(CONFIG);
    s_timeFmtIdx = (s_timeFmtIdx + 1) % kTimeFormatCount;
    CONFIG.timeFormat = kTimeFormats[s_timeFmtIdx];
}
static void cycle_time_left() {
    AwtrixConfig::Guard g(CONFIG);
    s_timeFmtIdx = (s_timeFmtIdx + kTimeFormatCount - 1) % kTimeFormatCount;
    CONFIG.timeFormat = kTimeFormats[s_timeFmtIdx];
}
static const char *show_time_fmt() {
    /* Compact label: keep format string itself short enough for the matrix. */
    snprintf(s_buf, sizeof(s_buf), "%s", CONFIG.timeFormat.c_str());
    return s_buf;
}

/* ── DATE (format preset) ───────────────────────────────────── */
static void cycle_date_right() {
    AwtrixConfig::Guard g(CONFIG);
    s_dateFmtIdx = (s_dateFmtIdx + 1) % kDateFormatCount;
    CONFIG.dateFormat = kDateFormats[s_dateFmtIdx];
}
static void cycle_date_left() {
    AwtrixConfig::Guard g(CONFIG);
    s_dateFmtIdx = (s_dateFmtIdx + kDateFormatCount - 1) % kDateFormatCount;
    CONFIG.dateFormat = kDateFormats[s_dateFmtIdx];
}
static const char *show_date_fmt() {
    snprintf(s_buf, sizeof(s_buf), "%s", CONFIG.dateFormat.c_str());
    return s_buf;
}

/* ── WEEKDAY (start on Monday) ──────────────────────────────── */
static void tog_som() { AwtrixConfig::Guard g(CONFIG); CONFIG.startOnMonday = !CONFIG.startOnMonday; }
static const char *show_som() { return CONFIG.startOnMonday ? "MON" : "SUN"; }

/* ── TEMP (°C / °F) ─────────────────────────────────────────── */
static void tog_cel() { AwtrixConfig::Guard g(CONFIG); CONFIG.isCelsius = !CONFIG.isCelsius; }
static const char *show_cel() { return CONFIG.isCelsius ? "C" : "F"; }

/* ── APPS (cycle visible native apps; right = enable, left = disable) ── */
static bool *apps_field(int idx) {
    switch (idx) {
        case 0: return &CONFIG.showTime;
        case 1: return &CONFIG.showDate;
        case 2: return &CONFIG.showTemp;
        case 3: return &CONFIG.showHum;
        case 4: return &CONFIG.showBat;
    }
    return nullptr;
}
static const char *apps_label(int idx) {
    switch (idx) {
        case 0: return "Time"; case 1: return "Date"; case 2: return "Temp";
        case 3: return "Hum";  case 4: return "Bat";
    }
    return "?";
}
static void apps_cycle_right() {
    /* Single press → advance to next slot. Toggle is handled separately via
     * left/right when on this slot in original, but the IDF port keeps it
     * simple: right toggles the current slot ON. */
    AwtrixConfig::Guard g(CONFIG);
    bool *f = apps_field(s_appsIdx);
    if (f) *f = true;
    s_appsIdx = (s_appsIdx + 1) % 5;
    DisplayManager::get().loadNativeApps();
}
static void apps_cycle_left() {
    AwtrixConfig::Guard g(CONFIG);
    bool *f = apps_field(s_appsIdx);
    if (f) *f = false;
    s_appsIdx = (s_appsIdx + 4) % 5;
    DisplayManager::get().loadNativeApps();
}
static const char *show_apps() {
    bool *f = apps_field(s_appsIdx);
    snprintf(s_buf, sizeof(s_buf), "%s:%s", apps_label(s_appsIdx),
             (f && *f) ? "ON" : "OFF");
    return s_buf;
}

/* ── SOUND (soundActive) ────────────────────────────────────── */
static void tog_sound() { AwtrixConfig::Guard g(CONFIG); CONFIG.soundActive = !CONFIG.soundActive; }
static const char *show_sound() { return fmt_bool(CONFIG.soundActive); }

/* ── VOLUME (0..30) ─────────────────────────────────────────── */
static void inc_vol()   { AwtrixConfig::Guard g(CONFIG); if (CONFIG.soundVolume < 30) CONFIG.soundVolume++; }
static void dec_vol()   { AwtrixConfig::Guard g(CONFIG); if (CONFIG.soundVolume >  0) CONFIG.soundVolume--; }
static const char *show_vol() { return fmt_int(CONFIG.soundVolume); }

/* ── SwBtn / Rotate (carried over from previous port) ───────── */
static void tog_swap()  { AwtrixConfig::Guard g(CONFIG); CONFIG.swap_buttons  = !CONFIG.swap_buttons;  }
static const char *show_swap() { return fmt_bool(CONFIG.swap_buttons); }
static void tog_rot()   { AwtrixConfig::Guard g(CONFIG); CONFIG.rotate_screen = !CONFIG.rotate_screen; }
static const char *show_rot()  { return fmt_bool(CONFIG.rotate_screen);  }

static const MenuItem ITEMS[] = {
    /* Order mirrors the original BRIGHT → VOLUME chain, plus SwBtn/Rotate
     * which only existed in the IDF port (UPDATE was removed in pack
     * planning since OTA is out-of-scope). */
    { "BRIGHT",  dec_bri,           inc_bri,           show_bri      },
    { "COLOR",   cycle_color_left,  cycle_color_right, show_color    },
    { "SWITCH",  tog_atrans,        tog_atrans,        show_atrans   },
    { "T-SPEED", dec_tspeed,        inc_tspeed,        show_tspeed   },
    { "APPTIME", dec_atime,         inc_atime,         show_atime    },
    { "TIME",    cycle_time_left,   cycle_time_right,  show_time_fmt },
    { "DATE",    cycle_date_left,   cycle_date_right,  show_date_fmt },
    { "WEEKDAY", tog_som,           tog_som,           show_som      },
    { "TEMP",    tog_cel,           tog_cel,           show_cel      },
    { "APPS",    apps_cycle_left,   apps_cycle_right,  show_apps     },
    { "SOUND",   tog_sound,         tog_sound,         show_sound    },
    { "VOLUME",  dec_vol,           inc_vol,           show_vol      },
    { "SwBtn",   tog_swap,          tog_swap,          show_swap     },
    { "Rotate",  tog_rot,           tog_rot,           show_rot      },
};
static const int ITEM_COUNT = (int)(sizeof(ITEMS) / sizeof(ITEMS[0]));

static bool s_active = false;
static int  s_idx = 0;

} // anonymous namespace

extern "C" {

void awtrix_menu_enter(void) {
    /* Sync the cycle indices to whatever CONFIG currently holds so the
     * menu starts on the user's existing selection rather than 0. */
    s_active = true; s_idx = 0;
    for (int i = 0; i < kTextColorCount; ++i)
        if (kTextColors[i] == CONFIG.textColor888) { s_colorIdx = i; break; }
    for (int i = 0; i < kTimeFormatCount; ++i)
        if (CONFIG.timeFormat == kTimeFormats[i]) { s_timeFmtIdx = i; break; }
    for (int i = 0; i < kDateFormatCount; ++i)
        if (CONFIG.dateFormat == kDateFormats[i]) { s_dateFmtIdx = i; break; }
    s_appsIdx = 0;
    ESP_LOGI(TAG, "Menu opened");
}

void awtrix_menu_exit(void) {
    s_active = false;
    CONFIG.save();
    DisplayManager::get().applyAllSettings();
    ESP_LOGI(TAG, "Menu closed (saved)");
}

bool awtrix_menu_active(void) { return s_active; }

void awtrix_menu_left(void) {
    if (!s_active) return;
    if (ITEMS[s_idx].onLeft) ITEMS[s_idx].onLeft();
}

void awtrix_menu_right(void) {
    if (!s_active) return;
    if (ITEMS[s_idx].onRight) ITEMS[s_idx].onRight();
}

void awtrix_menu_select_short(void) {
    if (!s_active) return;
    s_idx = (s_idx + 1) % ITEM_COUNT;
}

void awtrix_menu_select_long(void) {
    if (s_active) awtrix_menu_exit();
    else          awtrix_menu_enter();
}

const char *awtrix_menu_label(void) {
    if (!s_active || s_idx < 0 || s_idx >= ITEM_COUNT) return "";
    static char out[40];
    snprintf(out, sizeof(out), "%s:%s",
             ITEMS[s_idx].label,
             ITEMS[s_idx].display ? ITEMS[s_idx].display() : "?");
    return out;
}

} // extern "C"
