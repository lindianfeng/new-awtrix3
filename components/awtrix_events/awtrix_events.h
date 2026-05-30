/**
 * awtrix_events — header-only event bus that breaks the
 * `awtrix_core` ↔ `awtrix_periphery` cyclic dependency.
 *
 * Both components used to #include each other's headers (DisplayManager.h
 * from periphery; awtrix_io.h from core), creating a circular DAG that
 * CMake tolerates only because each side lists the other in PRIV_REQUIRES.
 * That coupling makes future refactors brittle: you can never compile one
 * side without the other.
 *
 * The event bus turns those direct calls into late-bound std::function
 * slots. `main` is the only place that wires both sides together at boot:
 *
 *     auto &bus = AwtrixEventBus::get();
 *     bus.onRtttlRequest    = [](const char *r){ awtrix_rtttl_play(r); };
 *     bus.onShowSleepScreen = []{ DisplayManager::get().showSleepAnimation(); };
 *     bus.onSetBrightness   = [](int b){ DisplayManager::get().setBrightness(b); };
 *
 * After this rewire, `awtrix_core` no longer #includes `awtrix_io.h`, and
 * `awtrix_periphery` no longer #includes `DisplayManager.h`. Each side
 * simply calls bus.onFoo(...) (with a nullptr check).
 *
 * Header-only on purpose: zero runtime cost, no extra .a, no link-order
 * issues. The single `static AwtrixEventBus instance` lives in this header
 * via the Meyer's-singleton pattern.
 */

#pragma once

#ifdef __cplusplus

#include <functional>
#include <string>

/* ── Event bus ───────────────────────────────────────────────────
 * All slots default-construct to "empty" std::function; callers MUST
 * null-check before invoking (mirrors the original Arduino pattern of
 * "callback may not be set if the dependent module is disabled").
 *
 * Slot naming convention:
 *   onXyzRequest  — periphery is the producer, core consumes (e.g. button event)
 *   onXyzAction   — core is the producer, periphery consumes (e.g. play sound)
 */
struct AwtrixEventBus
{
    /* ── Core → Periphery slots ──────────────────────────────────
     * Filled in by main() after PeripheryManager::setup(). The core uses
     * these whenever a notification asks for sound/RTTTL/etc. */
    std::function<void(const char* rtttl)> onRtttlAction; /* play RTTTL string */
    std::function<void(const char* json)> onSoundAction; /* play DFPlayer sound (json: {"id":n}) */
    std::function<void(const char* msg)> onR2D2Action; /* synthesize R2-D2-style buzzer beeps */
    std::function<void(uint8_t v0_30)> onSetVolumeAction; /* DFPlayer / buzzer volume */

    /* ── Periphery → Core slots ──────────────────────────────────
     * Filled in by main() after DisplayManager::setup(). The periphery
     * uses these to push UI updates back to the display. */
    std::function<void()> onShowSleepScreen; /* before deep sleep */
    std::function<void(int brightness_0_255)> onSetBrightness; /* auto-brightness ramp */
    std::function<void(uint8_t source,
                       const char *json)> onNotifyRequest; /* periphery-originated notify */

    /* ── Helpers (do nothing if slot is empty) ───────────────────
     * Inline so the optimizer can fully elide the call when the slot
     * is unset, exactly like a virtual call to a zero-impl shim. */
    void rtttl(const char* r) const { if (onRtttlAction) onRtttlAction(r); }
    void sound(const char* json) const { if (onSoundAction) onSoundAction(json); }
    void r2d2(const char* msg) const { if (onR2D2Action) onR2D2Action(msg); }
    void setVolume(uint8_t v) const { if (onSetVolumeAction) onSetVolumeAction(v); }
    void showSleepScreen() const { if (onShowSleepScreen) onShowSleepScreen(); }
    void setBrightness(int b) const { if (onSetBrightness) onSetBrightness(b); }
    void notify(uint8_t s, const char* j) const { if (onNotifyRequest) onNotifyRequest(s, j); }

    /* Meyer's singleton — thread-safe lazy init under C++11. */
    static AwtrixEventBus& get()
    {
        static AwtrixEventBus instance;
        return instance;
    }

private:
    AwtrixEventBus() = default;
    AwtrixEventBus(const AwtrixEventBus&) = delete;
    AwtrixEventBus& operator=(const AwtrixEventBus&) = delete;
};

/* Short alias used throughout the codebase: `EVENTS.rtttl(...)`. */
#define EVENTS (AwtrixEventBus::get())

#endif /* __cplusplus */
