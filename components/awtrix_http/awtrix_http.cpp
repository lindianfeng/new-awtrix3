#include "awtrix_http.h"
#include "awtrix_hal.h"
#include "awtrix_globals.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <esp_log.h>
#include <esp_spiffs.h>

static const char *TAG = TAG_HTTP;

/* ── route descriptor ─────────────────────────────────────────── */
struct route_t {
    std::string uri;
    httpd_method_t method;
    AwtrixHttpServer::Handler handler;
};

static std::vector<route_t> s_routes;

AwtrixHttpServer::AwtrixHttpServer() {}
AwtrixHttpServer::~AwtrixHttpServer() { stop(); }

/* ── global handler ───────────────────────────────────────────── */

/* Hardening 12: per-client-IP token bucket rate limiter. Any LAN host
 * can still reach the device (the firmware doesn't try to do firewalling),
 * but they can't loop on /api/notify or /api/settings hard enough to
 * starve the command queue or wear out NVS.
 *
 * Bucket policy:
 *   - 20 tokens capacity (burst)
 *   - 10 tokens/second refill (sustained rate)
 *   - 1 token per request
 *
 * Storage: 8-slot LRU table. The threat model is "casual LAN scanner /
 * misbehaving HA automation", not a botnet — 8 distinct attackers per
 * second is far past anything realistic on a home network. When the
 * table fills, the least-recently-used entry is evicted.
 *
 * Whitelisted URIs:
 *   - /setup, /scan, /connect — the AP-mode provisioning flow where the
 *     user is mashing buttons on the wizard page.
 *   - /api/screen, /screen, /fullscreen — the live-preview pages poll
 *     every 200 ms; legitimate, and read-only.
 *
 * Critical sections are short (a couple of array index ops + a timestamp
 * compare) so a portMUX is the right tool — no I/O under the lock. */

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

static constexpr int kRateBuckets        = 8;
static constexpr int kRateBucketCapacity = 20;
static constexpr int kRateRefillPerSec   = 10;

struct RateLimitEntry {
    uint32_t ip;              /* IPv4 in network byte order; 0 = empty slot */
    int      tokens;          /* current credit */
    uint64_t last_refill_us;  /* esp_timer monotonic when we last refilled */
    uint64_t last_seen_us;    /* esp_timer monotonic of last hit (LRU) */
};

static RateLimitEntry s_rate_table[kRateBuckets] = {};
static portMUX_TYPE   s_rate_mux = portMUX_INITIALIZER_UNLOCKED;

static bool rate_limit_whitelisted(const char *uri) {
    if (!uri) return false;
    return (strcmp(uri, "/setup") == 0
         || strcmp(uri, "/scan") == 0
         || strcmp(uri, "/connect") == 0
         || strcmp(uri, "/api/screen") == 0
         || strcmp(uri, "/screen") == 0
         || strcmp(uri, "/fullscreen") == 0);
}

/* Returns true if the request should be served; false if it should be
 * rejected with HTTP 429. Sets *retry_seconds when returning false. */
static bool rate_limit_check(httpd_req_t *req, int &retry_seconds) {
    retry_seconds = 1;

    if (rate_limit_whitelisted(req->uri)) return true;

    /* Resolve client IP. If we can't (test harness, weird transports),
     * fall open — we'd rather serve legit traffic than 500 everything. */
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) return true;
    struct sockaddr_storage peer = {};
    socklen_t plen = sizeof(peer);
    if (getpeername(sockfd, (struct sockaddr *)&peer, &plen) != 0) return true;
    uint32_t ip = 0;
    if (peer.ss_family == AF_INET) {
        ip = ((struct sockaddr_in *)&peer)->sin_addr.s_addr;
    } else {
        /* IPv6 — hash the last 32 bits to share buckets without
         * cross-protocol collisions. Good enough for the 8-slot table. */
        const uint8_t *a = ((struct sockaddr_in6 *)&peer)->sin6_addr.s6_addr;
        ip = ((uint32_t)a[12] << 24) | ((uint32_t)a[13] << 16)
           | ((uint32_t)a[14] <<  8) |  (uint32_t)a[15];
    }
    if (ip == 0) return true; /* would collide with the "empty slot" sentinel */

    const uint64_t now = (uint64_t)esp_timer_get_time();

    portENTER_CRITICAL(&s_rate_mux);

    /* Find this IP or the LRU slot. */
    int slot = -1;
    int lru  = 0;
    for (int i = 0; i < kRateBuckets; ++i) {
        if (s_rate_table[i].ip == ip) { slot = i; break; }
        if (s_rate_table[i].ip == 0)  { slot = i; }
        else if (s_rate_table[lru].last_seen_us > s_rate_table[i].last_seen_us) {
            lru = i;
        }
    }
    if (slot < 0) slot = lru;
    RateLimitEntry &e = s_rate_table[slot];

    if (e.ip != ip) {
        /* Fresh slot or eviction. */
        e.ip             = ip;
        e.tokens         = kRateBucketCapacity;
        e.last_refill_us = now;
    } else {
        /* Refill since last hit. last_refill_us = now after refill, so a
         * burst doesn't pay refill twice. */
        const uint64_t elapsed = now - e.last_refill_us;
        const int refill = (int)((elapsed * (uint64_t)kRateRefillPerSec) / 1000000ULL);
        if (refill > 0) {
            e.tokens += refill;
            if (e.tokens > kRateBucketCapacity) e.tokens = kRateBucketCapacity;
            e.last_refill_us += (uint64_t)refill * (1000000ULL / kRateRefillPerSec);
        }
    }

    e.last_seen_us = now;
    bool allow;
    if (e.tokens > 0) {
        e.tokens--;
        allow = true;
    } else {
        allow = false;
        /* Approx whole seconds until 1 token is back. */
        retry_seconds = 1 + (kRateRefillPerSec > 0 ? 0 : 0);
    }

    portEXIT_CRITICAL(&s_rate_mux);
    return allow;
}

esp_err_t AwtrixHttpServer::static_handler(httpd_req_t *req) {
    /* Hardening 12: rate-limit before doing any real work. The 429 path
     * is cheap (no body alloc, no auth check) so a flood is dropped at
     * the door instead of using up command-queue / NVS budget. */
    int retry_seconds = 1;
    if (!rate_limit_check(req, retry_seconds)) {
        char retry_after[16];
        snprintf(retry_after, sizeof(retry_after), "%d", retry_seconds);
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_hdr(req, "Retry-After", retry_after);
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "rate limited", 12);
        ESP_LOGW(TAG, "Rate limited %s", req->uri);
        return ESP_OK;   /* keep socket open, don't 500 */
    }

    for (auto &r : s_routes) {
        if (r.uri != req->uri) continue;
        if (static_cast<int>(r.method) != HTTP_ANY &&
            static_cast<int>(r.method) != req->method) continue;
        if (r.handler) return r.handler(req);
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

/* ── start / stop ─────────────────────────────────────────────── */
bool AwtrixHttpServer::start(uint16_t port) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 64;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&m_handle, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return false;
    }

    /* register catch-all */
    httpd_uri_t fallback = {};
    fallback.uri      = "/*";
    fallback.method   = static_cast<httpd_method_t>(HTTP_ANY);
    fallback.handler  = static_handler;
    fallback.user_ctx = nullptr;
    httpd_register_uri_handler(m_handle, &fallback);
    ESP_LOGI(TAG, "HTTP server started on port %d", port);
    return true;
}

void AwtrixHttpServer::stop() {
    if (m_handle) { httpd_stop(m_handle); m_handle = nullptr; }
}

/* ── route helpers ────────────────────────────────────────────── */
void AwtrixHttpServer::on(const std::string &uri, httpd_method_t method, Handler handler) {
    s_routes.push_back({uri, method, std::move(handler)});
}

void AwtrixHttpServer::serveStatic(const std::string &uri, const std::string &filePath) {
    on(uri, HTTP_GET, [filePath](httpd_req_t *req) -> esp_err_t {
        std::string fullPath = "/spiffs" + filePath;
        FILE *fp = fopen(fullPath.c_str(), "rb");
        if (!fp) { httpd_resp_send_404(req); return ESP_FAIL; }
        if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); httpd_resp_send_500(req); return ESP_FAIL; }
        long fsize = ftell(fp);
        if (fsize < 0) { fclose(fp); httpd_resp_send_500(req); return ESP_FAIL; }
        rewind(fp);
        char *buf = (char *)malloc(fsize);
        if (!buf) { fclose(fp); httpd_resp_send_500(req); return ESP_FAIL; }
        size_t n = fread(buf, 1, fsize, fp);
        fclose(fp);
        auto ends_with = [](const std::string &s, const char *suffix) {
            size_t sl = strlen(suffix);
            return s.size() >= sl && s.compare(s.size() - sl, sl, suffix) == 0;
        };
        const char *ct = "application/octet-stream";
        if (ends_with(filePath, ".html") || ends_with(filePath, ".htm")) ct = "text/html";
        else if (ends_with(filePath, ".js"))   ct = "application/javascript";
        else if (ends_with(filePath, ".css"))  ct = "text/css";
        else if (ends_with(filePath, ".json")) ct = "application/json";
        else if (ends_with(filePath, ".png"))  ct = "image/png";
        else if (ends_with(filePath, ".jpg") || ends_with(filePath, ".jpeg")) ct = "image/jpeg";
        else if (ends_with(filePath, ".gif"))  ct = "image/gif";
        else if (ends_with(filePath, ".ico"))  ct = "image/x-icon";
        httpd_resp_set_type(req, ct);
        httpd_resp_send(req, buf, n);
        free(buf);
        return ESP_OK;
    });
}

void AwtrixHttpServer::onText(const std::string &uri, httpd_method_t method,
                               const std::string &content, const std::string &contentType) {
    on(uri, method, [content, contentType](httpd_req_t *req) -> esp_err_t {
        httpd_resp_set_type(req, contentType.c_str());
        httpd_resp_send(req, content.c_str(), content.size());
        return ESP_OK;
    });
}

/* ── request helpers ──────────────────────────────────────────── */

/* Bug 1: hard ceiling on any body read. Caller-level helpers
 * (`read_limited_body` in awtrix_api.cpp) impose tighter per-route
 * limits, but this is defense in depth: even if a new route forgets
 * to call them, the body can never grow past 1 MiB. Anything bigger
 * is treated as an attack / misconfiguration and dropped on the floor.
 *
 * The two integer-overflow guards below matter because `req->content_len`
 * comes straight from the HTTP `Content-Length:` header — an attacker
 * fully controls it. */
static constexpr size_t kMaxBodyHardLimit = 1 * 1024 * 1024; /* 1 MiB */

std::string AwtrixHttpServer::getBody(httpd_req_t *req) {
    /* Reject negative / suspicious lengths up front. The esp_http_server
     * internals already refuse Transfer-Encoding: chunked, but the field
     * type is signed, so a malicious client could send 0xFFFFFFFF and
     * end up with content_len < 0. */
    if (req->content_len <= 0) return "";
    const size_t total_len = (size_t)req->content_len;
    if (total_len > kMaxBodyHardLimit) {
        ESP_LOGE(TAG, "getBody: Content-Length %u exceeds hard limit %u",
                 (unsigned)total_len, (unsigned)kMaxBodyHardLimit);
        return "";
    }

    /* total_len + 1 cannot overflow because total_len <= 1 MiB. */
    char *buf = (char *)malloc(total_len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "getBody: malloc(%u+1) failed", (unsigned)total_len);
        return "";
    }
    int received = httpd_req_recv(req, buf, total_len);
    std::string result;
    if (received > 0) { buf[received] = 0; result = buf; }
    free(buf);
    return result;
}

std::string AwtrixHttpServer::getHeader(httpd_req_t *req, const std::string &name) {
    size_t len = httpd_req_get_hdr_value_len(req, name.c_str());
    if (len == 0) return "";
    /* P1-10: zero-init the buffer so a partial / failed read can't leak
     * heap residue back to the client. */
    char *buf = (char *)calloc(len + 1, 1);
    if (!buf) return "";
    esp_err_t err = httpd_req_get_hdr_value_str(req, name.c_str(), buf, len + 1);
    std::string result;
    if (err == ESP_OK) {
        result.assign(buf);
    } else {
        ESP_LOGW(TAG, "getHeader(%s) failed: %s", name.c_str(), esp_err_to_name(err));
    }
    free(buf);
    return result;
}

std::string AwtrixHttpServer::getQueryParam(httpd_req_t *req, const std::string &key) {
    size_t len = httpd_req_get_url_query_len(req) + 1;
    if (len <= 1) return "";
    /* P1-10: same zero-init + return-code check as getHeader(). */
    char *buf = (char *)calloc(len, 1);
    if (!buf) return "";
    if (httpd_req_get_url_query_str(req, buf, len) != ESP_OK) {
        free(buf);
        return "";
    }

    std::string result;
    const size_t klen = key.size();
    /* Boundary-aware search: a key matches only when it sits at the start of
     * the query or immediately after '&', and is followed by '='. This avoids
     * prefix pollution (e.g. searching "ab" inside "abc=1&ab=2"). */
    char *p = buf;
    while (p && *p) {
        bool atBoundary = (p == buf) || (*(p - 1) == '&');
        if (atBoundary && strncmp(p, key.c_str(), klen) == 0 && p[klen] == '=') {
            char *valStart = p + klen + 1;
            char *end = strchr(valStart, '&');
            if (end) *end = '\0';
            result = valStart;
            break;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    free(buf);
    return result;
}

/* ── send helpers ─────────────────────────────────────────────── */
esp_err_t AwtrixHttpServer::sendText(httpd_req_t *req, const char *text, int code) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_status(req, std::to_string(code).c_str());
    httpd_resp_send(req, text, strlen(text));
    return ESP_OK;
}

esp_err_t AwtrixHttpServer::sendJson(httpd_req_t *req, const char *json, int code) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, std::to_string(code).c_str());
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

esp_err_t AwtrixHttpServer::sendStatus(httpd_req_t *req, int code) {
    httpd_resp_set_status(req, std::to_string(code).c_str());
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* Pack M+: serve a gzip-compressed HTML blob (used by /setup). The browser
 * decodes Content-Encoding: gzip transparently, so we just push the raw
 * compressed bytes and let it inflate client-side. */
esp_err_t AwtrixHttpServer::sendGzipHtml(httpd_req_t *req, const void *buf, size_t len) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    /* Cache-busting is not strictly required for setup but matches the
     * upstream esp-fs-webserver default. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, static_cast<const char *>(buf), len);
    return ESP_OK;
}