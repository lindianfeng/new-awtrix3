/**
 * awtrix_events — header-only event bus for *immediate hardware actions*.
 *
 * After round 8 / E1, this bus carries only fire-and-forget calls that
 * peripheral hardware should execute right away (play RTTTL, set buzzer
 * volume). Anything that affects display state, brightness, sleep mode
 * or user notifications now flows through `awtrix_command_bus_post()`
 * instead, so the main loop sees a single ordered command stream and the
 * codebase has one unambiguous answer to "where do I send this?".
 *
 *   Use the event bus       (this file)        for "do it now, no UI side effect"
 *   Use the command bus     (awtrix_command_bus.h) for "queue and let DisplayManager
 *                                                       process it next tick"
 *
 * Original cycle-breaking purpose still applies: this bus lets
 * `awtrix_display::awtrix_notification_overlay` call `playRTTTL` without
 * dragging `awtrix_periphery` into `awtrix_display`'s REQUIRES.
 *
 * Header-only on purpose: zero runtime cost, no extra .a, no link-order
 * issues. The single `static AwtrixEventBus instance` lives in this header
 * via the Meyer's-singleton pattern.
 */

#pragma once

#ifdef __cplusplus

#include <functional>
#include <string>

/* ── Event bus (immediate hardware actions only) ─────────────────
 * All slots default-construct to "empty" std::function; callers MUST
 * null-check before invoking. main() wires both ends at boot.
 *
 * The four slots below all forward to PeripheryManager. They're kept on
 * the event bus (rather than the command bus) for two reasons:
 *   - They're called from inside per-frame render loops where round-trip
 *     latency through the FreeRTOS queue would be a regression
 *     (notification rtttl in particular needs to start playing immediately
 *     when the notification appears, not on the next display tick).
 *   - They have no display-state side effects, so the command-bus
 *     serialisation guarantee that protects UI consistency isn't useful here.
 */
struct AwtrixEventBus {
    /* ── Display → Periphery: immediate-action slots ─────────────
     * Filled in by main() after PeripheryManager::setup(). The display
     * layer uses these whenever a notification asks for sound/RTTTL/etc. */
    std::function<void(const char *rtttl)>     onRtttlAction;     /* play RTTTL string */
    std::function<void(const char *json)>      onSoundAction;     /* play DFPlayer sound (json: {"id":n}) */
    std::function<void(const char *msg)>       onR2D2Action;      /* synthesize R2-D2-style buzzer beeps */
    std::function<void(uint8_t v0_30)>         onSetVolumeAction; /* DFPlayer / buzzer volume */

    /* ── Helpers (do nothing if slot is empty) ───────────────────
     * Inline so the optimizer can fully elide the call when the slot
     * is unset, exactly like a virtual call to a zero-impl shim. */
    void rtttl(const char *r) const          { if (onRtttlAction)     onRtttlAction(r); }
    void sound(const char *json) const       { if (onSoundAction)     onSoundAction(json); }
    void r2d2(const char *msg) const         { if (onR2D2Action)      onR2D2Action(msg); }
    void setVolume(uint8_t v) const          { if (onSetVolumeAction) onSetVolumeAction(v); }

    /* Meyer's singleton — thread-safe lazy init under C++11. */
    static AwtrixEventBus &get() {
        static AwtrixEventBus instance;
        return instance;
    }

private:
    AwtrixEventBus() = default;
    AwtrixEventBus(const AwtrixEventBus &) = delete;
    AwtrixEventBus &operator=(const AwtrixEventBus &) = delete;
};

/* Short alias used throughout the codebase: `EVENTS.rtttl(...)`. */
#define EVENTS (AwtrixEventBus::get())

#endif /* __cplusplus */
