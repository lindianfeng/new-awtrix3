#pragma once

#ifdef __cplusplus

#include <string>

/* P1-D: MQTT placeholder registry — port of Functions::replacePlaceholders +
 * subscribeToPlaceholders from the original Arduino firmware.
 *
 * customApp text fields may embed {{some/mqtt/topic}}; parseCustomPage scans
 * each new payload, calls awtrix_placeholder_register() for every topic it
 * finds, and the MQTT layer subscribes to those topics. Inbound messages
 * call awtrix_placeholder_update() with the raw payload, and the renderer
 * substitutes the latest value at draw time via awtrix_placeholder_get().
 *
 * Lives in awtrix_kernel so all three users (DisplayManager renderer,
 * parseCustomPage, awtrix_mqtt::process_message) get the same translation
 * unit without dragging awtrix_network into awtrix_core (or vice versa).
 *
 * Threading: the underlying map is guarded by a portMUX critical section
 * because the MQTT writer runs on the mqtt_client task while the renderer
 * reads from the display task. Both paths are short and lock-free outside
 * the critical region.
 *
 * Caps:
 *   - At most 32 simultaneously-registered topics (after that register()
 *     becomes a no-op). 32 covers every realistic customApp dashboard.
 *   - Each cached value is truncated to 96 bytes — long sensor payloads
 *     wouldn't fit on the 32x8 matrix anyway.
 */

bool awtrix_placeholder_register(const std::string& topic);
void awtrix_placeholder_update(const std::string& topic, const std::string& value);
bool awtrix_placeholder_has(const std::string& topic);
std::string awtrix_placeholder_get(const std::string& topic);

#endif /* __cplusplus */
