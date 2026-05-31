#include "awtrix_notification_overlay.h"

#include "awtrix_globals.h"
#include "awtrix_command_bus.h"  /* E1: post SetBrightness for wakeup notifications */
#include "awtrix_notifications.h"
#include "effects_core.h"   /* callEffect / EffectOverlay — now in this same
                              component after round 5, so the previous
                              __attribute__((weak)) reverse-link hack is gone. */
#include "awtrix_icon_loader.h"  /* Pack K+L: 8x8 RGB565 / JPEG icon loader */
#include "awtrix_color_utils.h"  /* A1: shared hsv_to_rgb / lerp_color / apply_fx /
                                    apply_case / scale_chart — used to be three
                                    copies, now one. */
#include "awtrix_events.h"        /* EVENTS.rtttl — immediate-action only after E1 */
#include "esp_timer.h"
#include <cctype>

/* ── Pack C: small helpers (file-local) ────────────────────────
 * The original NotifyOverlay is ~280 lines; we keep the structure but
 * factor the common knobs (text case, gradient, rainbow, fade/blink)
 * into tiny inline lambdas/helpers so the main lambda stays readable.
 *
 * A1 update: the five HSV/lerp/fx/case/chart helpers that used to live
 * here are now in <awtrix_color_utils.h> so customApp + notification
 * + Functions all share one definition. The local code below just calls
 * `awtrix_color::*`. */

const AwtrixNotificationOverlayCallback& awtrix_notification_overlay()
{
    static AwtrixNotificationOverlayCallback overlay = [](Matrix& m, UiState&, GifPlayer*)
    {
        /* Bug 6: deferred-I/O snapshot. EVENTS.rtttl() chains into
         * PeripheryManager::playRTTTL → awtrix_rtttl_play() which touches
         * SPIFFS/NVS, but the queue critical section below is a portMUX
         * spinlock that forbids I/O (see docs/THREADING.md §anti-patterns).
         * We capture the desired sound under the lock and dispatch after
         * the lock releases. Empty string = "nothing to play this frame". */
        std::string rtttl_to_play;
        {
            /* M2: lock the notification queue for the full critical section so
         * a concurrent HTTP /api/notify or MQTT enqueue/dismiss cannot
         * invalidate our front()/erase iterators. The critical region runs
         * ~50 µs (a few hundred matrix pixel writes) which is well inside
         * the portMUX comfort zone. */
            AwtrixNotificationManager::Lock _qlock(NOTIFICATIONS.mux());
            auto& q = NOTIFICATIONS.queue();
            if (q.empty()) return;
            AwtrixNotification& n = q.front();

            const int W = m.width();
            const int H = m.height();
            const long nowMs = (long)(esp_timer_get_time() / 1000);

            if (n.startTime == 0)
            {
                n.startTime = nowMs;
                n.scrollPosition = (float)W;
                /* Pack C: wakeup ramps the brightness back up the moment the
             * notification appears. E1: command-bus post (DisplayManager is
             * the single consumer) instead of EventBus→main→command-bus. */
                if (n.wakeup)
                {
                    AwtrixCommand command;
                    command.type = AwtrixCommandType::SetBrightness;
                    command.source = AWTRIX_COMMAND_SOURCE_SYSTEM;
                    command.payload = std::to_string(CONFIG.brightness);
                    awtrix_command_bus_post(command, 0);
                }
            }

            long elapsedMs = nowMs - n.startTime;

            /* Background fill */
            m.fillRect(0, 0, W, H, n.bgColor ? n.bgColor : 0x000000);

            /* Optional FX background + per-app weather overlay */
            if (n.effect >= 0) callEffect(m, 0, 0, n.effect);
            if (CONFIG.globalOverlay > OVERLAY_TIME)
            {
                EffectOverlay(m, 0, 0, (int)CONFIG.globalOverlay);
            }

            /* Sound: play once on first frame. loopSound just re-arms each
         * cycle by clearing the played flag (cheap heuristic — driver
         * may also support native looping in pack K).
         *
         * Bug 6: capture the sound string here instead of calling
         * EVENTS.rtttl() directly — playRTTTL() ends up doing flash I/O
         * which deadlocks under the portMUX. We replay after the lock. */
            if (n.rtttl && !n.soundPlayed)
            {
                if (!n.sound.empty()) rtttl_to_play = n.sound;
                n.soundPlayed = true;
            }
            else if (n.loopSound && n.rtttl && elapsedMs > 0 && (elapsedMs % 3000) < 50)
            {
                /* re-fire roughly every 3s while the notification is up */
                if (!n.sound.empty()) rtttl_to_play = n.sound;
            }

            /* Pack K+L: optional 8x8 icon at the left edge (same loader as
         * customApp). Text rendering below reserves 9+iconOffset px so
         * the icon doesn't get overdrawn. */
            bool iconReserved = false;
            if (!n.iconName.empty())
            {
                uint16_t iconPx[64];
                if (awtrix_icon_load_rgb565(n.iconName.c_str(), iconPx))
                {
                    m.drawRGBBitmap(0, 0, iconPx, 8, 8);
                    iconReserved = true;
                }
            }
            int iconReserve = iconReserved ? (9 + n.iconOffset) : 0;
            int textAreaW = W - iconReserve;

            /* Progress bar takes precedence over text (1 row at the bottom). */
            if (n.progress >= 0)
            {
                int p = n.progress;
                if (p > 100) p = 100;
                int w = (p * W) / 100;
                m.fillRect(0, H - 1, W, 1, n.progressBC);
                m.fillRect(0, H - 1, w, 1, n.progressC);
            }

            /* Bar / line chart. Both share the bottom (H-1) rows; autoscale into
         * [0..barH]. Line variant connects neighbouring points with drawLine. */
            const int barH = H - 1;
            if (!n.bar.empty())
            {
                std::vector<int> scaled;
                awtrix_color::scale_chart(n.bar, scaled, barH, n.autoscale);
                if (n.barBC) m.fillRect(0, 0, W, H, n.barBC);
                int wPer = W / (int)scaled.size();
                for (size_t i = 0; i < scaled.size(); ++i)
                {
                    int x = (int)i * wPer;
                    int h = scaled[i];
                    m.fillRect(x, H - h, wPer - 1, h, n.color ? n.color : CONFIG.textColor888);
                }
            }
            if (!n.line.empty())
            {
                std::vector<int> scaled;
                awtrix_color::scale_chart(n.line, scaled, barH, n.autoscale);
                if (n.barBC && n.bar.empty()) m.fillRect(0, 0, W, H, n.barBC);
                int wPer = (scaled.size() > 1) ? W / ((int)scaled.size() - 1) : W;
                uint32_t lc = n.color ? n.color : CONFIG.textColor888;
                for (size_t i = 1; i < scaled.size(); ++i)
                {
                    int x0 = (int)(i - 1) * wPer;
                    int x1 = (int)i * wPer;
                    int y0 = H - scaled[i - 1];
                    int y1 = H - scaled[i];
                    m.drawLine(x0, y0, x1, y1, lc);
                }
            }

            /* Text rendering: case-aware, optional rainbow / gradient / fade-blink.
         * Anchor row: topText → y=0, else y=1, plus textOffset. progress
         * pushes baseline up by 1 so it doesn't overlap the bar. */
            std::string cased;
            awtrix_color::apply_case(n.text, n.textCase, cased);
            int textW = (int)cased.size() * 4;
            int baseY = (n.topText ? 0 : 1) + n.textOffset;
            if (n.progress >= 0 && baseY > 0) baseY = 0;

            /* Decide horizontal position. center forces center; otherwise scroll
         * iff text doesn't fit. iconReserve shifts everything right by 9+offset. */
            const bool fits = (textW <= textAreaW);
            const bool centered = n.center || fits || n.noScrolling;
            int xStart;
            if (centered)
            {
                xStart = iconReserve + (textAreaW - textW) / 2;
                if (xStart < iconReserve) xStart = iconReserve;
            }
            else
            {
                xStart = iconReserve + (int)n.scrollPosition;
            }

            /* Decide per-character color: rainbow > gradient > fragments > base */
            uint32_t baseCol = n.color ? n.color : CONFIG.textColor888;
            baseCol = awtrix_color::apply_fx(baseCol, n.fadeText, n.blinkText, nowMs);

            if (!cased.empty())
            {
                if (n.rainbow)
                {
                    /* Cycle hue across characters and frames. */
                    uint8_t h = (uint8_t)((nowMs / 16) & 0xFF);
                    int cx = xStart;
                    for (char ch : cased)
                    {
                        char buf[2] = {ch, 0};
                        uint32_t col = awtrix_color::hsv_to_rgb(h, 255, 255);
                        col = awtrix_color::apply_fx(col, n.fadeText, n.blinkText, nowMs);
                        m.setTextColor(col);
                        m.setCursor(cx, baseY);
                        m.print(buf);
                        cx += 4;
                        h += 8;
                    }
                }
                else if (n.gradient[0] != n.gradient[1])
                {
                    /* Per-character linear interpolation between gradient[0..1]. */
                    int len = (int)cased.size();
                    int cx = xStart;
                    for (int i = 0; i < len; ++i)
                    {
                        uint8_t t = (len > 1) ? (uint8_t)((i * 255) / (len - 1)) : 0;
                        uint32_t col = awtrix_color::lerp_color(n.gradient[0], n.gradient[1], t);
                        col = awtrix_color::apply_fx(col, n.fadeText, n.blinkText, nowMs);
                        char buf[2] = {cased[i], 0};
                        m.setTextColor(col);
                        m.setCursor(cx, baseY);
                        m.print(buf);
                        cx += 4;
                    }
                }
                else if (!n.fragments.empty())
                {
                    /* Multi-color fragments. We keep glyph widths uniform at 4 px
                 * to stay consistent with the rest of the renderer. */
                    int cx = xStart;
                    for (size_t i = 0; i < n.fragments.size(); ++i)
                    {
                        std::string fc;
                        awtrix_color::apply_case(n.fragments[i], n.textCase, fc);
                        uint32_t col = (i < n.colors.size() && n.colors[i]) ? n.colors[i] : baseCol;
                        col = awtrix_color::apply_fx(col, n.fadeText, n.blinkText, nowMs);
                        m.setTextColor(col);
                        m.setCursor(cx, baseY);
                        m.print(fc.c_str());
                        cx += (int)fc.size() * 4;
                    }
                }
                else
                {
                    m.setTextColor(baseCol);
                    m.setCursor(xStart, baseY);
                    m.print(cased.c_str());
                }
            }

            /* Scroll advancement (only when text didn't fit and centering is off). */
            if (!centered)
            {
                float spd = (n.scrollSpeed > 0)
                                ? (float)n.scrollSpeed
                                : (float)CONFIG.scroll_speed;
                if (spd <= 0.0f) spd = 100.0f;
                n.scrollPosition -= awtrix_movement_factor * (spd / 100.0f);
                if (n.scrollPosition < -textW)
                {
                    n.scrollPosition = (float)W;
                    if (n.repeat > 0)
                    {
                        n.repeat--;
                        if (n.repeat == 0)
                        {
                            q.erase(q.begin());
                            /* Bug 6: drop any pending sound — the notification
                         * is gone, replaying its sound after the queue
                         * pop would be confusing. The next frame will
                         * pick up whatever (if anything) is now at the
                         * front of the queue. */
                            rtttl_to_play.clear();
                            return;
                        }
                    }
                }
            }

            /* Duration-based auto-dismiss. duration<=0 means "hold forever"
         * (used by hold:true or wakeup:true notifications). */
            if (n.repeat == -1 && n.duration > 0 && elapsedMs >= (long)n.duration * 1000L)
            {
                q.erase(q.begin());
            }
        } /* end portMUX critical section — _qlock destructed here */

        /* Bug 6: now safe to do the deferred sound I/O. EVENTS.rtttl() can
         * touch SPIFFS/NVS without deadlocking the queue lock. */
        if (!rtttl_to_play.empty()) EVENTS.rtttl(rtttl_to_play.c_str());
    };
    return overlay;
}
