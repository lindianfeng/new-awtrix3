#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ── MQTT manager (port of src/MQTTManager.cpp using esp-mqtt) ──
 * Lifecycle:
 *   awtrix_mqtt_init()   — call after Wi-Fi is up
 *   awtrix_mqtt_tick()   — call every loop iteration (housekeeping)
 *   awtrix_mqtt_publish(topic_suffix, payload)
 *   awtrix_mqtt_subscribe(topic_suffix)
 *
 * All topics use the configured MQTT_PREFIX (defaults to the unique device id)
 * as the namespace, matching the original AWTRIX3 conventions.
 */

void awtrix_mqtt_init(void);
void awtrix_mqtt_tick(void);
bool awtrix_mqtt_is_connected(void);
void awtrix_mqtt_publish(const char* topic_suffix, const char* payload);
void awtrix_mqtt_subscribe(const char* topic_suffix);

/* HomeAssistant Discovery: publishes the device + light/notify/sensor configs
 * to the HA_PREFIX namespace so HomeAssistant auto-discovers the device. */
void awtrix_mqtt_publish_ha_discovery(void);

/* Hook used by DisplayManager::sendAppLoop (weak symbol). */
void awtrix_mqtt_publish_app_loop(const char* app_name);

#ifdef __cplusplus
}
#endif
