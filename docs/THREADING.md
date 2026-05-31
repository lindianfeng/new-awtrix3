# THREADING.md — concurrency model & lock conventions

This document captures the locking discipline for every shared mutable object
in the firmware. The build runs on **ESP32-S3** with **2 cores** and **3+
FreeRTOS tasks** that all touch the singleton state:

| Task / source         | Files                                                                    | Cadence       |
|-----------------------|--------------------------------------------------------------------------|---------------|
| `main` loop           | `main/app_main.cpp`                                                      | ~62 Hz tick   |
| Wi-Fi event dispatcher| esp_event default task                                                   | bursty        |
| MQTT client callback  | `awtrix_network/awtrix_mqtt.cpp::mqtt_event_handler`                     | per msg       |
| HTTP request handlers | `awtrix_http/awtrix_api.cpp::h_*`                                        | per request   |
| iot_button callbacks  | `awtrix_periphery/awtrix_periphery.cpp::_button_event_cb`                | per press     |
| Art-Net receive task  | `awtrix_network/awtrix_artnet.cpp` background task                       | bursty        |
| btncb worker task     | `awtrix_http/awtrix_api.cpp::btn_cb_worker_task`                         | bursty        |

If you add a new task or a new shared object, please update this file in the
same change.

---

## Sharing model

We deliberately use **`portMUX_TYPE` (FreeRTOS spinlock) for short critical
sections** and **`SemaphoreHandle_t` (recursive mutex) only where the
critical region calls into NVS / SPIFFS / cJSON** — anywhere the holder
might block on flash I/O. The two-tier choice is repeated for every shared
object below.

`portMUX_TYPE` is roughly 10× cheaper than a FreeRTOS mutex and safe to
take from any task (we never take it from an ISR). It does **not** allow
the holder to call `vTaskDelay` / file I/O / network I/O — that would
hang the whole core. Whenever a critical section needs blocking work,
copy data out under the spinlock, release, then do the I/O.

---

## Shared objects

### 1. `AwtrixConfig` (70-field persisted config singleton)
- File: `awtrix_config/awtrix_globals.h`, `awtrix_globals.cpp`
- Lock: `m_lock` — recursive `SemaphoreHandle_t` (opaque `void *` in
  the header per A3; cast back in the .cpp).
- API: `AwtrixConfig::Guard guard(CONFIG);` RAII helper.
- Why mutex (not spinlock): `load()` / `save()` / `toJson()` all do NVS I/O
  or cJSON allocation under the lock. Recursive so a caller can already
  hold the lock and pass through a helper that re-takes it.

### 2. `DisplayManager` (UI state + app rotation)
- File: `awtrix_core/DisplayManager.h`
- Lock: `m_dataLock` — `portMUX_TYPE`.
- API: `DisplayManager::Lock _l(&m_dataLock);` RAII helper.
- Protects: `s_moodlightColor`, `s_moodlightActive`, `m_currentCustomApp`,
  the AppRegistry vector/map mutations driven from HTTP/MQTT (load
  custom apps, reorder, eraseCustomApp).

### 3. `AwtrixAppRegistry` (apps + customApps lookup)
- File: `awtrix_kernel/awtrix_apps.h`, `awtrix_kernel/awtrix_apps.cpp`
- Lock: **none of its own** — caller holds `DisplayManager::m_dataLock`.
- Rationale: every mutator is reached through `DisplayManager` methods
  (`parseCustomPage`, `reorderApps`, `loadNativeApps`) which already
  take the lock at the call site. Avoids a double-lock.

### 4. `AwtrixNotificationManager` (notification FIFO) — **M2**
- File: `awtrix_display/awtrix_notifications.h`, `.cpp`
- Lock: `m_mux` — `portMUX_TYPE`.
- API: `AwtrixNotificationManager::Lock _l(NOTIFICATIONS.mux());`
- Self-locking mutators: `enqueue`, `replaceHead`, `dismiss`, `empty`.
- `queue()` returns a *mutable* reference; callers MUST RAII-lock around
  the read **and** any subsequent writes. The notification overlay
  takes the lock at the top of its lambda (~50 µs critical section).

### 5. `PeripheryManager::m_btnLock` (button event ring buffer)
- File: `awtrix_periphery/awtrix_periphery.h`
- Lock: `m_btnLock` — `portMUX_TYPE`.
- Protects: `m_pendingBtnEvent[4]`, `m_veryLongFired[4]`.
- Writers: `_button_event_cb` (iot_button task), via
  `PeripheryManager::enqueueBtnEvent` / `clearVeryLongFired`.
- Reader: `PeripheryManager::tick()` (main loop).

### 6. `AwtrixDisplaySnapshot` (RGB888 frame for /api/screen)
- File: `awtrix_kernel/awtrix_display_snapshot.h`, `.cpp`
- Lock: `m_lock` — `portMUX_TYPE`.
- Writers: `DisplayManager::tick()` after `m_matrix->show()`.
- Readers: HTTP `/api/screen` / `/fullscreen` handlers.

### 7. `awtrix_placeholders` (MQTT topic → latest value cache)
- File: `awtrix_kernel/awtrix_placeholders.cpp`
- Lock: file-local `Registry::lock` — `portMUX_TYPE`.
- Writers: MQTT data callback (`awtrix_mqtt.cpp::process_message`).
- Readers: customApp renderer in `DisplayManager_customApp.cpp`.

### 8. `awtrix_artnet` frame buffer
- File: `awtrix_network/awtrix_artnet.cpp`
- Lock: `s_frame_lock` — `portMUX_TYPE`.
- Writer: receive task `recv()` callback.
- Reader: `DisplayManager::tick()` Art-Net branch via
  `awtrix_artnet_take_frame()`.

### 9. `AwtrixCommand` queue (FreeRTOS Queue) — **lock-free**
- File: `awtrix_kernel/awtrix_command_bus.cpp`
- Lock: none — FreeRTOS `xQueueSend` / `xQueueReceive` are intrinsically
  thread-safe.
- Writers: HTTP / MQTT / button event handlers / periphery
  auto-brightness / notification overlay wakeup.
- Reader: `DisplayManager::processPendingEvents()` in the display tick.

### 10. `awtrix::StateMachine` (system state) — **L1, atomic**
- File: `awtrix_kernel/awtrix_system_state.cpp`
- Lock: none — `std::atomic<uint8_t>` with acquire/release ordering.
- Writers: WiFi event handler, `DisplayManager::moodlight`/`setPower`,
  `awtrix_power_sleep`.
- Readers: anyone — `SYS_STATE` macro is a single atomic load.

---

## Anti-patterns to avoid

* **Calling a NotificationManager mutator while holding
  `NOTIFICATIONS.mux()`** — the mutator takes the same portMUX, which
  on ESP32 spinlocks is OK (re-entrant on the same core) but on cross-core
  IDF builds will deadlock. Always release first, then call the mutator.
* **Holding `m_dataLock` across a network call** — DisplayManager's
  lock is a portMUX; blocking IO under it deadlocks the other core.
  Snapshot the data, release, then dispatch.
* **Lock ordering inversion**: the canonical order is
  `m_dataLock` ⟶ `CONFIG.m_lock`. Never take them in the other order.
  (Look at `DisplayManager::moodlight()` for the pattern: outer
  `Lock _l(&m_dataLock)`, inner `AwtrixConfig::Guard cfgGuard(CONFIG)`.)
* **Reading `queue()` without holding `NOTIFICATIONS.mux()`** —
  HTTP `/api/notify` can resize the vector while you iterate.

---

## How to add a new shared object

1. Pick the lock type using this decision tree:
   - Critical section does no I/O AND fits in <100 µs → `portMUX_TYPE`.
   - Critical section calls NVS / SPIFFS / cJSON / network → recursive
     `SemaphoreHandle_t` mutex.
   - Single primitive read/write → `std::atomic<T>` (no lock).
2. Add a RAII `Lock` helper (copy from any of the existing objects).
3. Document it in the **Shared objects** table above.
4. If the critical section ever needs to mix two locks, add the pair to
   the **anti-patterns** section so the next developer doesn't invert.
