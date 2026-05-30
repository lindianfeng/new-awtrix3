/**
 * Port of src/MenuManager.cpp. Original menu was an Adafruit-GFX overlay;
 * this port keeps the same item list (Brightness, AutoBrightness, FPS,
 * Transition, AppTime, ...) and uses CONFIG fields for state.
 */
#include "awtrix_menumanager.h"
#include "awtrix_globals.h"
#include "DisplayManager.h"
#include "esp_log.h"

#define TAG "menu"

namespace
{
    struct MenuItem
    {
        const char* label;
        void (*onLeft)();
        void (*onRight)();
        const char*(*display)();
    };

    static char s_buf[16];

    static const char* fmt_int(int v)
    {
        snprintf(s_buf, sizeof(s_buf), "%d", v);
        return s_buf;
    }

    static const char* fmt_bool(bool v) { return v ? "ON" : "OFF"; }

    static void inc_bri() { CONFIG.brightness = (CONFIG.brightness < 250) ? CONFIG.brightness + 5 : 255; }
    static void dec_bri() { CONFIG.brightness = (CONFIG.brightness > 5) ? CONFIG.brightness - 5 : 0; }
    static const char* show_bri() { return fmt_int(CONFIG.brightness); }

    static void inc_abri() { CONFIG.auto_brightness = true; }
    static void dec_abri() { CONFIG.auto_brightness = false; }
    static const char* show_abri() { return fmt_bool(CONFIG.auto_brightness); }

    static void inc_fps() { if (CONFIG.matrix_fps < 60) CONFIG.matrix_fps++; }
    static void dec_fps() { if (CONFIG.matrix_fps > 10) CONFIG.matrix_fps--; }
    static const char* show_fps() { return fmt_int(CONFIG.matrix_fps); }

    static void inc_atrans() { CONFIG.auto_transition = true; }
    static void dec_atrans() { CONFIG.auto_transition = false; }
    static const char* show_atrans() { return fmt_bool(CONFIG.auto_transition); }

    static void inc_atime() { CONFIG.timePerApp += 500; }
    static void dec_atime() { if (CONFIG.timePerApp > 1000) CONFIG.timePerApp -= 500; }
    static const char* show_atime() { return fmt_int((int)(CONFIG.timePerApp / 1000)); }

    static void inc_swap() { CONFIG.swap_buttons = !CONFIG.swap_buttons; }
    static const char* show_swap() { return fmt_bool(CONFIG.swap_buttons); }

    static void inc_rot() { CONFIG.rotate_screen = !CONFIG.rotate_screen; }
    static const char* show_rot() { return fmt_bool(CONFIG.rotate_screen); }

    static const MenuItem ITEMS[] = {
        {"Bri", dec_bri, inc_bri, show_bri},
        {"ABri", dec_abri, inc_abri, show_abri},
        {"FPS", dec_fps, inc_fps, show_fps},
        {"ATrans", dec_atrans, inc_atrans, show_atrans},
        {"ATime", dec_atime, inc_atime, show_atime},
        {"SwBtn", inc_swap, inc_swap, show_swap},
        {"Rotate", inc_rot, inc_rot, show_rot},
    };
    static const int ITEM_COUNT = sizeof(ITEMS) / sizeof(ITEMS[0]);

    static bool s_active = false;
    static int s_idx = 0;
} // anonymous namespace

extern "C" {
void awtrix_menu_enter(void)
{
    s_active = true;
    s_idx = 0;
    ESP_LOGI(TAG, "Menu opened");
}

void awtrix_menu_exit(void)
{
    s_active = false;
    CONFIG.save();
    DisplayManager::get().applyAllSettings();
    ESP_LOGI(TAG, "Menu closed (saved)");
}

bool awtrix_menu_active(void) { return s_active; }

void awtrix_menu_left(void)
{
    if (!s_active) return;
    if (ITEMS[s_idx].onLeft) ITEMS[s_idx].onLeft();
}

void awtrix_menu_right(void)
{
    if (!s_active) return;
    if (ITEMS[s_idx].onRight) ITEMS[s_idx].onRight();
}

void awtrix_menu_select_short(void)
{
    if (!s_active) return;
    s_idx = (s_idx + 1) % ITEM_COUNT;
}

void awtrix_menu_select_long(void)
{
    if (s_active) awtrix_menu_exit();
    else awtrix_menu_enter();
}

const char* awtrix_menu_label(void)
{
    if (!s_active || s_idx < 0 || s_idx >= ITEM_COUNT) return "";
    static char out[24];
    snprintf(out, sizeof(out), "%s:%s",
             ITEMS[s_idx].label,
             ITEMS[s_idx].display ? ITEMS[s_idx].display() : "?");
    return out;
}
} // extern "C"
