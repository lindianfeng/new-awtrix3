#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "awtrix_hal.h"
#include "esp_netif.h"
#include "esp_event.h"

/* ── Wi-Fi manager ─────────────────────────────────────────────── */
void awtrix_wifi_start(void);
bool awtrix_wifi_is_ready(void);
bool awtrix_wifi_is_connected(void);
esp_ip4_addr_t awtrix_wifi_get_ip(void);
void awtrix_wifi_get_ip_str(char *buf, size_t len);
void awtrix_wifi_set_hostname(const char *name);

/* ── mDNS ─────────────────────────────────────────────────────── */
void awtrix_mdns_init(const char *hostname, const char *unique_id);
void awtrix_mdns_add_service(const char *proto, const char *service, uint16_t port);

/* ── SNTP ─────────────────────────────────────────────────────── */
void awtrix_sntp_init(const char *server, const char *tz);

/* ── UDP (FIND_AWTRIX responder) ──────────────────────────────── */
void awtrix_udp_discovery_init(uint16_t port);
void awtrix_udp_discovery_tick(const char *hostname, uint16_t http_port);

#ifdef __cplusplus
}
#endif