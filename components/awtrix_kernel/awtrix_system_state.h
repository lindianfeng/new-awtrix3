#pragma once

#ifdef __cplusplus

#include <stdint.h>

/**
 * awtrix_system_state — explicit device-level state machine.
 *
 * Before round 8 / L1 the device's overall mode was implicit, derived from
 * four scattered booleans:
 *     CONFIG.ap_mode          — soft-AP fallback, no WiFi credentials yet
 *     CONFIG.artnetMode       — Art-Net DMX receiver hijacks the matrix
 *     CONFIG.matrix_off       — display blanked by /api/power off
 *     s_moodlightActive       — DisplayManager-local solid color mode
 * plus the display-layer AwtrixDisplayMode enum, which only described
 * the render layer (not whether WiFi was up or whether we were heading
 * to deep sleep).
 *
 * The implicit mix made two things hard:
 *   1. **Logging / debugging.** "What state is the device actually in?"
 *      required reading 4 different fields in different modules.
 *   2. **Adding new modes.** A new behaviour (e.g. "OTA in progress")
 *      had to invent yet another flag and remember to switch every
 *      module that already branched on the existing ones.
 *
 * This header defines a single, comprehensive system state, plus a tiny
 * thread-safe state machine that DisplayManager / main / awtrix_network
 * all consult through the same API. The implementation is header-only
 * because it carries no state of its own beyond a uint8_t inside the
 * Meyer's-singleton AwtrixStateMachine instance.
 *
 *   AwtrixSystemState::current()
 *       — read-only snapshot; safe to call from any task.
 *   AwtrixStateMachine::transition(next, reason)
 *       — atomic write + ESP_LOGI("disp", "state: A → B (reason)").
 *       Returns false if the requested transition is not allowed.
 *   AwtrixStateMachine::canTransitionTo(next)
 *       — preview the same predicate without mutating state.
 *
 * The legacy bool fields are NOT removed yet: existing call sites still
 * read them, and DisplayManager / awtrix_periphery write them. The
 * state machine is updated alongside those writes so callers can migrate
 * incrementally without a flag day. Once every consumer reads the state
 * machine instead, the legacy bools can be deleted in a follow-up.
 *
 * Layering: lives in awtrix_kernel so display / network / periphery
 * can all read it without depending on awtrix_core. Header-only keeps
 * the include cost down to a single byte of static storage.
 */

namespace awtrix {

/* Top-level device states. Ordering is roughly the boot lifeline; the
 * transition matrix in canTransitionTo() enforces the legal edges. */
enum class SystemState : uint8_t {
    Boot          = 0,  /* before main loop starts */
    APMode        = 1,  /* soft-AP fallback, /setup wizard active */
    Connecting    = 2,  /* STA join in progress */
    Normal        = 3,  /* app rotation + notifications */
    Moodlight     = 4,  /* solid-color full matrix (HA light entity) */
    ArtNet        = 5,  /* DMX-over-WiFi receiver writes pixels directly */
    PoweredOff    = 6,  /* /api/power off — matrix blank, services up */
    EnteringSleep = 7,  /* deep sleep transition pending */
    Error         = 8,  /* unrecoverable runtime error */
};

const char *to_string(SystemState s);

/* Returns true if `next` is a valid successor of `from`. Designed as a
 * static `constexpr` table so the compiler can fold the check into a
 * single load+compare at every call site. */
bool can_transition(SystemState from, SystemState next);

/* Singleton state machine. All access goes through .current() / .transition()
 * which are both lock-free thanks to the underlying std::atomic<uint8_t>. */
class StateMachine {
public:
    static StateMachine &get();

    /* Returns the current SystemState. Atomic read, safe from any task. */
    SystemState current() const;

    /* Atomic transition. Logs at INFO level if the move is allowed,
     * WARN if rejected (e.g. ArtNet → Sleep without going through Normal).
     * `reason` is folded into the log message; pass nullptr for none.
     * Returns true on success, false if the transition was rejected. */
    bool transition(SystemState next, const char *reason = nullptr);

    /* Force a transition regardless of canTransition(). Use sparingly —
     * intended for error recovery and reset paths only. Still logs.   */
    void force(SystemState next, const char *reason = nullptr);

private:
    StateMachine() = default;
    StateMachine(const StateMachine &) = delete;
    StateMachine &operator=(const StateMachine &) = delete;
};

} // namespace awtrix

/* Convenience macros so legacy code reads slightly cleaner:
 *     if (SYS_STATE == SystemState::APMode) { ... }
 *     SYS_TRANSITION(SystemState::Normal, "wifi_connected"); */
#define SYS_STATE      (awtrix::StateMachine::get().current())
#define SYS_TRANSITION(s, why) awtrix::StateMachine::get().transition((s), (why))

#endif /* __cplusplus */
