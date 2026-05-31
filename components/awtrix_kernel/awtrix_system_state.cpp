#include "awtrix_system_state.h"

#include <atomic>
#include "esp_log.h"

#define TAG "state"

namespace awtrix
{
    const char* to_string(SystemState s)
    {
        switch (s)
        {
        case SystemState::Boot: return "Boot";
        case SystemState::APMode: return "AP";
        case SystemState::Connecting: return "Connecting";
        case SystemState::Normal: return "Normal";
        case SystemState::Moodlight: return "Moodlight";
        case SystemState::ArtNet: return "ArtNet";
        case SystemState::PoweredOff: return "PoweredOff";
        case SystemState::EnteringSleep: return "EnteringSleep";
        case SystemState::Error: return "Error";
        }
        return "?";
    }

    /* Transition matrix. Each row lists the legal successors of a state.
 *
 * Two design rules baked into this table:
 *   - Any state can fall back to Error (for unrecoverable failures).
 *   - Any state can be reset to Boot only by a hard restart (we do not
 *     allow software transitions into Boot at runtime).
 *
 * Reading the table:
 *   row index = current state
 *   bit `1 << next` set ⇒ transition allowed.
 *
 * Mirrors the original AWTRIX behaviour: AP ↔ Connecting ↔ Normal is the
 * usual provisioning path; from Normal you can take any user-triggered
 * detour (Moodlight / ArtNet / PoweredOff / EnteringSleep); detours go
 * back to Normal when their toggle is cleared. */
    static constexpr uint32_t make_bit(SystemState s)
    {
        return 1u << static_cast<uint8_t>(s);
    }

    static constexpr uint32_t kAllowed[9] = {
        /* Boot          */ make_bit(SystemState::APMode)
        | make_bit(SystemState::Connecting)
        | make_bit(SystemState::Error),
        /* APMode        */ make_bit(SystemState::Connecting)
        | make_bit(SystemState::Error),
        /* Connecting    */ make_bit(SystemState::Normal)
        | make_bit(SystemState::APMode)
        | make_bit(SystemState::Error),
        /* Normal        */ make_bit(SystemState::Moodlight)
        | make_bit(SystemState::ArtNet)
        | make_bit(SystemState::PoweredOff)
        | make_bit(SystemState::EnteringSleep)
        | make_bit(SystemState::Connecting)
        | make_bit(SystemState::Error),
        /* Moodlight     */ make_bit(SystemState::Normal)
        | make_bit(SystemState::EnteringSleep)
        | make_bit(SystemState::Error),
        /* ArtNet        */ make_bit(SystemState::Normal)
        | make_bit(SystemState::EnteringSleep)
        | make_bit(SystemState::Error),
        /* PoweredOff    */ make_bit(SystemState::Normal)
        | make_bit(SystemState::EnteringSleep)
        | make_bit(SystemState::Error),
        /* EnteringSleep */ make_bit(SystemState::Normal) /* aborted */
        | make_bit(SystemState::Error),
        /* Error         */ make_bit(SystemState::Error), /* terminal; reboot to clear */
    };

    bool can_transition(SystemState from, SystemState next)
    {
        if (from == next) return true; /* idempotent re-assertion */
        uint8_t idx = static_cast<uint8_t>(from);
        if (idx >= sizeof(kAllowed) / sizeof(kAllowed[0])) return false;
        return (kAllowed[idx] & make_bit(next)) != 0;
    }

    /* The actual storage lives in this TU as an std::atomic so reads/writes
 * across the IDF tasks (display tick + main loop + MQTT callback +
 * HTTP handler) stay race-free without the singleton needing its own lock. */
    static std::atomic<uint8_t> g_state{static_cast<uint8_t>(SystemState::Boot)};

    StateMachine& StateMachine::get()
    {
        static StateMachine instance;
        return instance;
    }

    SystemState StateMachine::current() const
    {
        return static_cast<SystemState>(g_state.load(std::memory_order_acquire));
    }

    bool StateMachine::transition(SystemState next, const char* reason)
    {
        SystemState from = current();
        if (!can_transition(from, next))
        {
            ESP_LOGW(TAG, "rejected transition %s → %s%s%s",
                     to_string(from), to_string(next),
                     reason ? " (" : "", reason ? reason : "");
            return false;
        }
        if (from == next) return true; /* no-op; don't spam logs */
        g_state.store(static_cast<uint8_t>(next), std::memory_order_release);
        ESP_LOGI(TAG, "state: %s → %s%s%s",
                 to_string(from), to_string(next),
                 reason ? " (" : "", reason ? reason : "");
        return true;
    }

    void StateMachine::force(SystemState next, const char* reason)
    {
        SystemState from = current();
        if (from == next) return;
        g_state.store(static_cast<uint8_t>(next), std::memory_order_release);
        ESP_LOGW(TAG, "FORCED state: %s → %s%s%s",
                 to_string(from), to_string(next),
                 reason ? " (" : "", reason ? reason : "");
    }
} // namespace awtrix
