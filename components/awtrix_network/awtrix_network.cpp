#include "awtrix_network.h"
#include "awtrix_utils.h"
#include "awtrix_globals.h"
#include "awtrix_system_state.h"   /* L1: SYS_TRANSITION at each WiFi state change */
#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"               /* Hardening 11: esp_read_mac() for AP password derivation */
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"             /* P1-8: exponential reconnect backoff */
#include "mdns.h"
#include "lwip/sockets.h"
#include "freertos/event_groups.h"

static const char *TAG = TAG_NET;

/* ── Wi-Fi event group ──────────────────────────────────────── */
static EventGroupHandle_t s_wifi_evt = NULL;
#define WIFI_CONNECTED  BIT0
#define WIFI_FAIL       BIT1
#define WIFI_GOT_IP     BIT2

static esp_netif_t *s_sta_if = NULL;
static esp_netif_t *s_ap_if  = NULL;
static char s_hostname[DEFAULT_HOSTNAME_LEN];
static int  s_retry_count = 0;
static bool s_connected = false;
static bool s_ap_mode   = false;
static esp_ip4_addr_t s_my_ip;

/* P1-8: exponential backoff timer for STA reconnect attempts. The old
 * path called esp_wifi_connect() inline in the event handler, which
 * made the device burn through all 10 retries in ~1 s and then bounce
 * to AP mode. Worse, some routers see that pattern as a flood and start
 * dropping the next dozen association attempts entirely.
 *
 * We now schedule the reconnect via esp_timer with delays
 * 1s, 2s, 4s, 8s, 16s, 32s, 32s, 32s, 32s, 32s (capped) — total ~3 min
 * before falling back to AP. That gives routers time to reboot, DHCP
 * leases time to refresh, and rate-limited APs time to forgive us. */
static esp_timer_handle_t s_reconnect_timer = NULL;

static uint64_t wifi_backoff_us(int attempt) {
    /* attempt=0 → 1 s, attempt=1 → 2 s, attempt=2 → 4 s, … capped at 32 s. */
    int shift = attempt;
    if (shift > 5) shift = 5;
    return (uint64_t)(1000ULL * 1000ULL * (1ULL << shift));
}

static void wifi_reconnect_timer_cb(void *) {
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
            s_connected = false;
            if (s_retry_count < 10) {
                /* P1-8: schedule the reconnect rather than firing it
                 * inline. Delays back off exponentially (1/2/4/8/16/32 s
                 * capped) so a misbehaving router can't get DoS-flagged
                 * by us and we don't burn through all 10 retries in 1 s. */
                uint64_t delay_us = wifi_backoff_us(s_retry_count);
                s_retry_count++;
                ESP_LOGW(TAG, "Wi-Fi disconnected, retry %d in %llu ms (reason=%d)",
                         s_retry_count, (unsigned long long)(delay_us / 1000),
                         ev->reason);
                /* L1: still trying to join — declare we're in Connecting state
                 * so observers (display "AP MODE" splash, HA discovery) know
                 * not to assume Normal. */
                SYS_TRANSITION(awtrix::SystemState::Connecting, "sta_disconnected");
                if (s_reconnect_timer) {
                    /* Cancel any pending fire from a previous disconnect
                     * before re-arming, otherwise the timer just keeps
                     * running with the old delay. */
                    esp_timer_stop(s_reconnect_timer);
                    esp_timer_start_once(s_reconnect_timer, delay_us);
                } else {
                    /* Timer not yet created (shouldn't happen — we create
                     * it in awtrix_wifi_start before registering the
                     * handler). Fall back to immediate connect so we don't
                     * lose the retry. */
                    esp_wifi_connect();
                }
            } else {
                ESP_LOGE(TAG, "Wi-Fi failed after 10 retries, starting AP");
                s_ap_mode = true;
                esp_netif_ip_info_t ap_ip;
                esp_netif_get_ip_info(s_ap_if, &ap_ip);
                s_my_ip = ap_ip.ip;
                /* L1: AP fallback — /setup wizard is the only sensible UI
                 * until the user re-provisions credentials. */
                SYS_TRANSITION(awtrix::SystemState::APMode, "sta_retries_exhausted");
                xEventGroupSetBits(s_wifi_evt, WIFI_FAIL);
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "AP: station connected");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "AP: station disconnected");
            break;
        default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_retry_count = 0;
        /* P1-8: cancel any pending exponential-backoff reconnect attempt.
         * Without this, a timer armed during disconnect can still fire
         * after we're already connected, wasting an esp_wifi_connect()
         * call that the radio then has to reject. */
        if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
        s_connected = true;
        s_my_ip = ev->ip_info.ip;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&s_my_ip));
        /* L1: WiFi is up — head into the main render loop. */
        SYS_TRANSITION(awtrix::SystemState::Normal, "got_ip");
        xEventGroupSetBits(s_wifi_evt, WIFI_GOT_IP);
    }
}

void awtrix_wifi_start(void) {
    s_wifi_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_if = esp_netif_create_default_wifi_sta();
    s_ap_if  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    /* P1-8: create the exponential-backoff reconnect timer before any
     * WIFI_EVENT_STA_DISCONNECTED can fire. The timer callback runs in
     * the esp_timer task context, NOT the WiFi event task — that's
     * intentional so the reconnect itself never blocks event dispatch. */
    {
        esp_timer_create_args_t args = {};
        args.callback = wifi_reconnect_timer_cb;
        args.arg      = NULL;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name     = "wifi_reconn";
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_reconnect_timer));
    }

    wifi_config_t wifi_cfg = {};
    /* STA config from globals */
    auto &c = AwtrixConfig::get();
    if (!c.net_ip.empty() && !c.net_gw.empty() && !c.net_sn.empty()) {
        esp_netif_ip_info_t ip_info = {};
        inet_aton(c.net_ip.c_str(), &ip_info.ip);
        inet_aton(c.net_gw.c_str(), &ip_info.gw);
        inet_aton(c.net_sn.c_str(), &ip_info.netmask);
        esp_netif_dhcpc_stop(s_sta_if);
        esp_netif_set_ip_info(s_sta_if, &ip_info);
        if (!c.net_pdns.empty()) {
            esp_netif_dns_info_t dns = {};
            inet_aton(c.net_pdns.c_str(), (esp_ip4_addr_t *)&dns.ip);
            esp_netif_set_dns_info(s_sta_if, ESP_NETIF_DNS_MAIN, &dns);
        }
        ESP_LOGI(TAG, "Static IP: %s", c.net_ip.c_str());
    }
    /* Set hostname */
    awtrix_wifi_set_hostname(c.hostname.c_str());

    /* STA mode */

    /* STA SSID/PASS — read from NVS (keys set via /api/settings) */
    char ssid[33] = {0}, pass[65] = {0};
    awtrix_settings_load_str("WSSID", ssid, sizeof(ssid), "");
    awtrix_settings_load_str("WPASS", pass, sizeof(pass), "");
    if (ssid[0]) {
        /* P1-9: bounded copies into the IDF wifi_config_t fields.
         * SSID is 32 B (IDF SSID_LEN_MAX), PSK is 64 B (IDF PSK_LEN_MAX).
         * The source buffers above are sized to fit (ssid[33] = 32+NUL,
         * pass[65] = 64+NUL) so today's code doesn't overflow, but a
         * future tweak to the size constants would silently break the
         * old strcpy(). strncpy + explicit NUL is the standard
         * defense-in-depth pattern. */
        strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
        wifi_cfg.sta.ssid[sizeof(wifi_cfg.sta.ssid) - 1] = '\0';
        const char *src_pass = pass[0] ? pass : "";
        strncpy((char *)wifi_cfg.sta.password, src_pass, sizeof(wifi_cfg.sta.password) - 1);
        wifi_cfg.sta.password[sizeof(wifi_cfg.sta.password) - 1] = '\0';
    } else {
        ESP_LOGW(TAG, "No Wi-Fi SSID configured — AP mode only");
        s_ap_mode = true;
    }
    /* AP fallback config */
    wifi_config_t ap_cfg = {};
    strncpy((char *)ap_cfg.ap.ssid, c.hostname.c_str(), sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid[sizeof(ap_cfg.ap.ssid) - 1] = '\0';

    /* Hardening 11: derive a per-device AP password from the SoftAP MAC
     * instead of the hard-coded "12345678". 12345678 + LAN access to /files
     * = trivial filesystem leak the moment a stranger sits in WiFi range.
     *
     * The password format "AWX-XXXXXXXX" (12 chars total) satisfies WPA2's
     * 8-char minimum, is deterministic so the user can read it once and
     * keep using it across reboots, and varies per device because every
     * ESP32-S3 has a globally-unique factory-burned MAC.
     *
     * We log the password at boot (INFO) so the operator can grab it from
     * `idf.py monitor` during initial setup; once setup_htm reads it back
     * via /api/stats the operator can disable the log. */
    {
        uint8_t mac[6] = {0};
        esp_err_t merr = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        if (merr != ESP_OK) {
            /* Fallback to STA MAC if SOFTAP read fails (shouldn't happen). */
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
        }
        char appass[16];   /* "AWX-XXXXXXXX\0" = 13 chars, round to 16 */
        snprintf(appass, sizeof(appass), "AWX-%02X%02X%02X%02X",
                 mac[2], mac[3], mac[4], mac[5]);
        strcpy((char *)ap_cfg.ap.password, appass);
        ESP_LOGI(TAG, "AP password (derived from MAC): %s", appass);
    }

    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    /* power settings */
    esp_wifi_set_max_tx_power(80);
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (ssid[0]) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi started (non-blocking)");
}

bool awtrix_wifi_is_ready(void) {
    return s_connected || s_ap_mode;
}

bool awtrix_wifi_is_connected(void) { return s_connected && !s_ap_mode; }

esp_ip4_addr_t awtrix_wifi_get_ip(void) { return s_my_ip; }

void awtrix_wifi_get_ip_str(char *buf, size_t len) {
    snprintf(buf, len, IPSTR, IP2STR(&s_my_ip));
}

void awtrix_wifi_set_hostname(const char *name) {
    strncpy(s_hostname, name, sizeof(s_hostname) - 1);
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_sta_if, name));
}

/* ── mDNS ────────────────────────────────────────────────────── */
void awtrix_mdns_init(const char *hostname, const char *unique_id) {
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(hostname));
    ESP_LOGI(TAG, "mDNS hostname: %s.local", hostname);
}

void awtrix_mdns_add_service(const char *proto, const char *service, uint16_t port) {
    mdns_service_add(NULL, service, proto, port, NULL, 0);
}

/* ── SNTP ────────────────────────────────────────────────────── */
void awtrix_sntp_init(const char *server, const char *tz) {
    setenv("TZ", tz, 1);
    tzset();
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    esp_netif_sntp_init(&config);
    ESP_LOGI(TAG, "SNTP server=%s tz=%s", server, tz);
}

/* ── UDP discovery ───────────────────────────────────────────── */
static int s_udp_sock = -1;

void awtrix_udp_discovery_init(uint16_t port) {
    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp_sock < 0) { ESP_LOGE(TAG, "UDP socket failed"); return; }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    bind(s_udp_sock, (struct sockaddr *)&addr, sizeof(addr));
    fcntl(s_udp_sock, F_SETFL, O_NONBLOCK);
    ESP_LOGI(TAG, "UDP discovery on port %d", port);
}

void awtrix_udp_discovery_tick(const char *hostname, uint16_t http_port) {
    if (s_udp_sock < 0) return;
    char buf[256];
    struct sockaddr_in src;
    socklen_t srclen = sizeof(src);
    int len = recvfrom(s_udp_sock, buf, sizeof(buf) - 1, 0,
                       (struct sockaddr *)&src, &srclen);
    if (len > 0) {
        buf[len] = 0;
        if (strcmp(buf, "FIND_AWTRIX") == 0) {
            char reply[128];
            if (http_port != 80) snprintf(reply, sizeof(reply), "%s:%d", hostname, http_port);
            else                  snprintf(reply, sizeof(reply), "%s", hostname);
            sendto(s_udp_sock, reply, strlen(reply), 0,
                   (struct sockaddr *)&src, srclen);
            ESP_LOGI(TAG, "UDP discovery reply → %s", reply);
        }
    }
}