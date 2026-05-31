/**
 * DisplayManager_customApp.cpp — customApp renderer (split from
 * DisplayManager.cpp in round 7 / B2).
 *
 * Holds two things:
 *   1. `custom_replace_placeholders`  — file-static, customApp-specific
 *      `{{...}}` substitution that knows about CONFIG.* fields and the
 *      MQTT placeholder cache.
 *   2. `renderCustomApp`              — the ~250-line per-frame draw
 *      routine wired into APP_REGISTRY for every customApp slot.
 *
 * Why split? Editing the customApp pipeline used to recompile the entire
 * 2300-line DisplayManager.cpp; this file is ~290 lines, so changes to
 * scrolling / colour / icon logic touch ~12% of the original build cost.
 *
 * All shared state stays inside DisplayManager (m_dataLock, peekApps,
 * peekCustomApps, getUI) — those are already public, so this split needs
 * no friend declarations or new accessors.
 *
 * `renderCustomApp` is referenced from DisplayManager.cpp via a forward
 * declaration (in DisplayManager.cpp it appears as the value pushed into
 * APP_REGISTRY for each custom slot).
 */

#include "DisplayManager.h"
#include "awtrix_globals.h"
#include "awtrix_placeholders.h"   /* P1-D: MQTT topic placeholder registry */
#include "awtrix_color_utils.h"    /* A1: awtrix_color::* helpers */
#include "awtrix_icon_loader.h"    /* Pack K+L: SPIFFS 8x8 icon loader */
#include "awtrix_render.h"
#include "effects_core.h"
#include <esp_timer.h>
#include <functional>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

/* Symbol exported so DisplayManager.cpp can register it with APP_REGISTRY. */
void renderCustomApp(Matrix& m, UiState& state, int16_t x, int16_t y, GifPlayer*);

/* {{placeholder}} replacement: precedence is (1) MQTT topic registry, then
 * (2) CONFIG.field statics, then leave the literal in place. P1-D added
 * the MQTT layer so customApp text like "Temp {{home/sensor/temp}}" picks
 * up live values from the topic that parseCustomPage subscribed to.
 *
 * Kept file-static here (instead of lifted to awtrix_kernel) because it
 * directly references awtrix_core-owned CONFIG.* fields the kernel layer
 * has no business knowing about. */
static std::string custom_replace_placeholders(const std::string& in)
{
    if (in.find("{{") == std::string::npos) return in;
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size())
    {
        if (i + 1 < in.size() && in[i] == '{' && in[i + 1] == '{')
        {
            size_t end = in.find("}}", i + 2);
            if (end == std::string::npos)
            {
                out.append(in, i, std::string::npos);
                break;
            }
            std::string key = in.substr(i + 2, end - (i + 2));
            char numbuf[32];
            /* P1-D: live MQTT value wins over any static fallback. */
            if (awtrix_placeholder_has(key))
            {
                out += awtrix_placeholder_get(key);
            }
            else if (key == "CONFIG.brightness")
            {
                snprintf(numbuf, sizeof(numbuf), "%d", CONFIG.brightness);
                out += numbuf;
            }
            else if (key == "CONFIG.currentTemp")
            {
                snprintf(numbuf, sizeof(numbuf), "%.1f", CONFIG.currentTemp);
                out += numbuf;
            }
            else if (key == "CONFIG.currentHum")
            {
                snprintf(numbuf, sizeof(numbuf), "%.0f", CONFIG.currentHum);
                out += numbuf;
            }
            else if (key == "CONFIG.batteryPercent")
            {
                snprintf(numbuf, sizeof(numbuf), "%d", CONFIG.batteryPercent);
                out += numbuf;
            }
            else if (key == "CONFIG.currentApp") { out += CONFIG.currentApp; }
            else if (key == "CONFIG.hostname") { out += CONFIG.hostname; }
            else
            {
                out.append(in, i, end + 2 - i); /* leave unknown placeholder intact */
            }
            i = end + 2;
        }
        else
        {
            out += in[i++];
        }
    }
    return out;
}

void renderCustomApp(Matrix& m, UiState& state, int16_t x, int16_t y, GifPlayer*)
{
    auto& dm = DisplayManager::get();

    /* Resolve (slot index → app name → CustomApp) under the data lock and
     * keep a **pointer** so animation-state writes (scrollposition/repeat)
     * propagate back into the registry. The render itself is short and
     * happens under the lock — this is the same approach used by the
     * notification overlay queue front access. */
    std::string slotName;
    {
        DisplayManager::Lock _l(&dm.m_dataLock);
        const auto& apps = dm.peekApps();
        if (state.currentApp >= apps.size()) return;
        slotName = apps[state.currentApp].first;
    }

    DisplayManager::Lock _l(&dm.m_dataLock);
    /* peekCustomApps() returns const-ref, so go through the registry helper
     * to obtain a writable pointer. findCustomApp returns nullptr if the
     * slot is a native app (Time/Date/...) rather than a custom one. */
    CustomApp* appp = APP_REGISTRY.findCustomApp(slotName);
    if (!appp) return;
    CustomApp& app = *appp;

    const int W = m.width();
    const int H = m.height();
    const long nowMs = (long)(esp_timer_get_time() / 1000);

    /* (0) blink: skip the whole frame for half of every (2*blink) ms cycle. */
    if (app.blink > 0)
    {
        if (((nowMs / app.blink) & 1) != 0) return;
    }

    /* (1) Background + per-app FX + global weather overlay. */
    if (app.background) m.fillRect(x, y, W, H, app.background);
    if (app.effect >= 0) callEffect(m, x, y, app.effect);
    if (CONFIG.globalOverlay > OVERLAY_TIME)
        EffectOverlay(m, 0, 0, (int)CONFIG.globalOverlay);

    /* (1.5) Pack K+L: optional 8x8 icon at the left edge.
     * If pushIcon=1 the icon slides off the left as text begins to scroll;
     * if pushIcon=2 it comes back after one full scroll cycle (animation
     * driven by iconPosition / iconWasPushed). */
    bool iconReserved = false;
    if (!app.iconName.empty())
    {
        uint16_t iconPx[64];
        if (awtrix_icon_load_rgb565(app.iconName.c_str(), iconPx))
        {
            int iconX = x + (int)app.iconPosition;
            if (iconX > x - 8)
            {
                m.drawRGBBitmap(iconX, y, iconPx, 8, 8);
            }
            iconReserved = (iconX > x - 8);
        }
    }

    /* (2) Bar chart with autoscale. */
    if (app.barSize > 0)
    {
        std::vector<int> scaled;
        awtrix_color::scale_chart(app.barData, app.barSize, H - 1, app.autoscale, scaled);
        int slot = app.barSize > 0 ? W / app.barSize : W;
        if (slot < 1) slot = 1;
        uint32_t bc = app.hasCustomColor ? app.color : CONFIG.textColor888;
        for (int i = 0; i < (int)scaled.size(); i++)
        {
            int bx = x + i * slot;
            if (app.barBG) m.fillRect(bx, y, slot - 1, H, app.barBG);
            m.fillRect(bx, y + (H - scaled[i]), slot - 1, scaled[i], bc);
        }
    }
    /* (3) Line chart with autoscale. */
    if (app.lineSize > 1)
    {
        std::vector<int> scaled;
        awtrix_color::scale_chart(app.lineData, app.lineSize, H - 1, app.autoscale, scaled);
        int step = (W - 1) / (app.lineSize - 1);
        uint32_t lc = app.hasCustomColor ? app.color : CONFIG.textColor888;
        for (int i = 1; i < (int)scaled.size(); i++)
        {
            int y0 = y + (H - 1) - scaled[i - 1];
            int y1 = y + (H - 1) - scaled[i];
            m.drawLine(x + (i - 1) * step, y0, x + i * step, y1, lc);
        }
    }

    /* (4) drawInstructions DSL — uses the POD cache compiled in parseCustomPage. */
    if (!app.compiledDraw.empty())
    {
        DisplayManager::get().processCompiledDraw(x, y, app.compiledDraw, app.drawTexts);
    }

    /* (5) Progress bar at the bottom row. */
    if (app.progress >= 0 && app.progress <= 100)
    {
        int filled = (app.progress * W) / 100;
        int by = y + H - 1;
        if (app.pbColor) m.fillRect(x, by, W, 1, app.pbColor);
        if (filled > 0)
            m.fillRect(x, by, filled, 1, app.pColor
                                             ? app.pColor
                                             : (app.hasCustomColor ? app.color : CONFIG.textColor888));
    }

    /* (6) Text rendering. Substitute {{placeholder}}, apply textCase, then
     * decide fragments / rainbow / gradient / plain. */
    std::string raw = custom_replace_placeholders(app.text);
    if (app.textCase == 1)
    {
        for (auto& ch : raw) ch = (char)toupper((unsigned char)ch);
    }
    else if (app.textCase == 2)
    {
        for (auto& ch : raw) ch = (char)tolower((unsigned char)ch);
    }

    uint32_t baseCol = app.hasCustomColor ? app.color : CONFIG.textColor888;
    baseCol = awtrix_color::apply_fx(baseCol, app.fade, /*blinkMs already handled above*/ 0, nowMs);

    int textW = (int)raw.size() * 4;
    int yBase = (app.topText ? (y + 1) : (y + H - 7)) + app.textOffset;
    /* Pack K+L: when an icon is drawn, the text starts to its right. */
    int iconReserve = iconReserved ? (9 + app.iconOffset) : 0;
    int textAreaX = x + iconReserve;
    int textAreaW = W - iconReserve;

    /* Horizontal positioning: center for short text, otherwise scroll. */
    bool fits = (textW <= textAreaW);
    bool stayPut = app.center || app.noScrolling || fits;
    int xStart;
    if (stayPut)
    {
        xStart = app.center ? (textAreaX + (textAreaW - textW) / 2) : textAreaX;
        if (xStart < textAreaX) xStart = textAreaX;
    }
    else
    {
        xStart = textAreaX + (int)app.scrollposition;
    }

    auto draw_chars_at = [&](const std::string& s, int cx, uint32_t col,
                             std::function<uint32_t(int /*idx*/)> per_char_col)
    {
        for (size_t i = 0; i < s.size(); ++i)
        {
            uint32_t cc = per_char_col ? per_char_col((int)i) : col;
            m.setTextColor(cc);
            m.setCursor(cx, yBase);
            char buf[2] = {s[i], 0};
            m.print(buf);
            cx += 4;
        }
    };

    if (!app.fragments.empty())
    {
        /* Multi-color fragments — combined width for centering. */
        int totalW = 0;
        for (auto& s : app.fragments) totalW += (int)s.size() * 4;
        int cx = app.center ? (x + (W - totalW) / 2) : xStart;
        if (cx < x) cx = x;
        for (size_t i = 0; i < app.fragments.size(); ++i)
        {
            std::string fc = custom_replace_placeholders(app.fragments[i]);
            uint32_t col = (i < app.colors.size() && app.colors[i]) ? app.colors[i] : baseCol;
            col = awtrix_color::apply_fx(col, app.fade, 0, nowMs);
            m.setTextColor(col);
            m.setCursor(cx, yBase);
            m.print(fc.c_str());
            cx += (int)fc.size() * 4;
        }
    }
    else if (app.rainbow && !raw.empty())
    {
        uint8_t h = (uint8_t)((nowMs / 16) & 0xFF);
        draw_chars_at(raw, xStart, baseCol, [&](int)
        {
            uint32_t c = awtrix_color::hsv_to_rgb(h, 255, 255);
            h += 8;
            return awtrix_color::apply_fx(c, app.fade, 0, nowMs);
        });
    }
    else if (app.gradient[0] != app.gradient[1] && !raw.empty())
    {
        int len = (int)raw.size();
        draw_chars_at(raw, xStart, baseCol, [&](int i)
        {
            uint8_t t = (len > 1) ? (uint8_t)((i * 255) / (len - 1)) : 0;
            uint32_t c = awtrix_color::lerp_color((uint32_t)app.gradient[0], (uint32_t)app.gradient[1], t);
            return awtrix_color::apply_fx(c, app.fade, 0, nowMs);
        });
    }
    else if (!raw.empty())
    {
        m.setTextColor(baseCol);
        m.setCursor(xStart, yBase);
        m.print(raw.c_str());
    }

    /* (7) Scroll advancement + repeat counting. Mirrors original Apps.cpp. */
    if (!stayPut)
    {
        float spd = (app.scrollSpeed > 0) ? app.scrollSpeed : 100.0f;
        app.scrollposition -= awtrix_movement_factor * (spd / 100.0f);
        if (app.scrollposition < (float)(-textW))
        {
            app.scrollposition = (float)W;
            if (app.repeat > 0)
            {
                app.currentRepeat++;
                if (app.currentRepeat >= app.repeat)
                {
                    /* End of life: erase or just stop scrolling. lifeTimeEnd
                     * mode for non-lifetime apps not used by original; we
                     * simply remove the customApp slot. */
                    APP_REGISTRY.eraseCustomApp(slotName);
                    APP_REGISTRY.eraseApp(slotName);
                    if (dm.getUI()) dm.getUI()->setApps(APP_REGISTRY.apps());
                    return;
                }
            }
        }
    }

    /* (8) lifeTimeEnd red border (set by Pack J's checkLifetime path). */
    if (app.lifeTimeEnd)
    {
        m.drawRect(x, y, W, H, AwtrixColors::kLifetimeExpiredBorder);
    }
}
