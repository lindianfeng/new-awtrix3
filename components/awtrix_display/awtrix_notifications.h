#pragma once

#ifdef __cplusplus

#include "awtrix_hal.h"
#include <string>
#include <vector>

struct AwtrixNotification {
    std::string text;
    uint32_t    color = 0xFFFFFF;
    int         effect = -1;
    int         duration = 5;
    long        startTime = 0;
    uint32_t    bgColor = 0;
    bool        rtttl = false;
    bool        wakeup = false;
    bool        push = false;
    bool        stack = true;

    float       scrollPosition = 0.0f;
    bool        soundPlayed    = false;
    std::string sound;
    int         repeat          = -1;
    bool        noScrolling     = false;
    /* Pack R: per-notification scroll speed (0 = use global CONFIG.scroll_speed,
     * >0 = overrides as percent; matches original Notification.scrollSpeed
     * field semantics). Multiplied by awtrix_movement_factor at render. */
    int         scrollSpeed     = 0;

    /* ── Pack G: full notification field set ported from src/DisplayManager.cpp
     * generateNotification(). Names mirror the original JSON keys. Defaults
     * chosen so an empty constructor renders identically to the bare struct
     * before pack G. */
    bool        center      = false;     /* JSON "center"      — force-center text */
    bool        rainbow     = false;     /* JSON "rainbow"     — HSV text */
    int         pushIcon    = 0;         /* JSON "pushIcon"    — 0=none 1=push 2=push+come back */
    int         iconOffset  = 0;         /* JSON "iconOffset"  — extra px between icon and text */
    int         textCase    = 0;         /* JSON "textCase"    — 0=as-is 1=upper 2=lower */
    int         textOffset  = 0;         /* JSON "textOffset"  — Y offset added to baseline */
    bool        topText     = false;     /* JSON "topText"     — anchor text to top row */
    int         fadeText    = 0;         /* JSON "fadeText"    — fade interval ms (0=off) */
    int         blinkText   = 0;         /* JSON "blinkText"   — blink interval ms (0=off) */
    uint32_t    gradient[2] = {0, 0};    /* JSON "gradient"[2] — start/end RGB for gradient text */
    int         progress    = -1;        /* JSON "progress"    — 0..100, -1 = off */
    uint32_t    progressC   = 0x00FF00;  /* JSON "progressC"   — bar fill */
    uint32_t    progressBC  = 0x202020;  /* JSON "progressBC"  — bar background */
    std::vector<int>       bar;          /* JSON "bar"[N]      — bar chart data */
    std::vector<int>       line;         /* JSON "line"[N]     — line chart data */
    uint32_t    barBC       = 0;         /* JSON "barBC"       — bar/line bg color */
    bool        autoscale   = true;      /* JSON "autoscale"   — bar/line auto-fit 0..8px */
    std::string iconName;                /* JSON "icon"        — SPIFFS or base64 ref */
    std::string effectSettings;          /* JSON "effectSettings" — raw JSON, forwarded to fx */
    bool        loopSound   = false;     /* JSON "loopSound"   — loop the RTTTL/track */
    bool        hold        = false;     /* JSON "hold"        — keep notification on screen */
    std::vector<std::string> fragments;  /* JSON "text" array  — multi-color text fragments */
    std::vector<uint32_t>    colors;     /* JSON colors        — per-fragment colors */
};

class AwtrixNotificationManager {
public:
    static AwtrixNotificationManager &get();

    /* All mutators below acquire the internal portMUX critical section
     * themselves — call them lock-free from any task. */
    void enqueue(const AwtrixNotification &notification);
    void replaceHead(const AwtrixNotification &notification);
    void dismiss();
    bool empty() const;

    /* Direct queue access: returns a *mutable* reference to the underlying
     * vector so the notification overlay can mutate front()'s
     * scrollPosition / startTime / repeat counters in place.
     *
     * THREADING: callers MUST hold `Lock _l(NOTIFICATIONS.mux());` for the
     * full duration of any read OR write that follows the returned ref —
     * otherwise a concurrent HTTP/MQTT enqueue / dismiss can invalidate
     * the iterator. See M2 + docs/THREADING.md. */
    std::vector<AwtrixNotification> &queue();
    const std::vector<AwtrixNotification> &queue() const;

    /* Returns the internal portMUX so callers can RAII-lock around a
     * critical section that mixes a queue() read with mutating writes
     * (the notification overlay's main loop is the only legitimate user). */
    portMUX_TYPE *mux();

    /* RAII helper. Use as:
     *   AwtrixNotificationManager::Lock _l(NOTIFICATIONS.mux());
     *   if (!NOTIFICATIONS.queue().empty()) { ... }
     */
    struct Lock {
        portMUX_TYPE *m;
        explicit Lock(portMUX_TYPE *l) : m(l) { portENTER_CRITICAL(m); }
        ~Lock() { portEXIT_CRITICAL(m); }
        Lock(const Lock &) = delete;
        Lock &operator=(const Lock &) = delete;
    };

private:
    AwtrixNotificationManager() = default;
    std::vector<AwtrixNotification> m_queue;
    /* M2: protects every read and write on m_queue. All public mutators
     * take it themselves; queue() callers must wrap the access in
     * AwtrixNotificationManager::Lock(mux()). The portMUX is cheaper than
     * a FreeRTOS mutex and safe to take from any task / non-ISR context. */
    mutable portMUX_TYPE m_mux = portMUX_INITIALIZER_UNLOCKED;
};

#define NOTIFICATIONS (AwtrixNotificationManager::get())

#endif /* __cplusplus */
