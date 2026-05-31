/**
 * AWTRIX3 HTTP API: 28 endpoints from the original ServerManager.cpp +
 * file-management endpoints from lib/webserver. All previously-stubbed
 * routes are now connected to their backing implementations:
 *   /api/rtttl     -> PeripheryManager::playRTTTL()
 *   /api/sound     -> PeripheryManager::parseSound()
 *   /api/r2d2      -> PeripheryManager::r2d2()
 *   /api/moodlight -> DisplayManager::moodlight()
 *   /api/reorder   -> DisplayManager::reorderApps()
 *   /api/custom    -> DisplayManager::parseCustomPage()
 *
 * Newly added (from original lib/webserver):
 *   GET /fullscreen    HTML pixel viewer
 *   GET /backup        download settings JSON
 *   GET /list          SPIFFS file index
 *   POST /upload       multipart file upload
 *   POST /delete       remove a file
 *   GET /edit          inline file editor (returns text content)
 */
#include "awtrix_api.h"
#include "awtrix_globals.h"
#include "awtrix_utils.h"        /* Pack M+: awtrix_settings_save_str for h_connect */
#include "awtrix_command_bus.h"
#include "awtrix_protocol.h"
#include "awtrix_display_snapshot.h"
#include "awtrix_query_snapshot.h"
#include "awtrix_periphery.h"
#include "awtrix_power.h"
#include "awtrix_setup_html.h"   /* Pack M+: gzip-compressed AP-mode setup wizard */
#include "awtrix_edit_html.h"    /* P1-E:    gzip-compressed SPIFFS file editor */
#include <cJSON.h>
#include <esp_log.h>
#include <esp_spiffs.h>
#include <esp_wifi.h>            /* Pack M+: /scan + /connect use the STA APIs directly */
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = TAG_HTTP;

/* ── Forward declarations for all route handlers ─────────────── */
/* ── E3: Route forward declarations are generated from
 * awtrix_routes.def via an X-macro expansion. To add or remove a route,
 * edit awtrix_routes.def — the forward decl, registration, and route-
 * count log line all stay in sync automatically. */
#define ROUTE(method, path, fn, auth) static esp_err_t fn(httpd_req_t *req);
#include "awtrix_routes.def"
#undef ROUTE

/* h_root is special-cased: it's not in the route table because it's a
 * "front door" that redirects to /setup in AP mode and serves a nav
 * page in STA mode. Keeping it out of the table also avoids needing a
 * "fall-through to default" entry. */
static esp_err_t h_root(httpd_req_t *req);

/* HTTP Basic-auth gate (defined at end of file). Returns false and sends
 * 401+WWW-Authenticate when CONFIG.auth_user is set and the request fails
 * authentication. Sensitive POST handlers call this first. */
bool awtrix_http_require_auth(httpd_req_t *req);

static constexpr int kMaxJsonBodyBytes = 16 * 1024;
static constexpr int kMaxUploadBytes = 256 * 1024;
static constexpr size_t kMaxSpiffsRelativePath = 96;

static esp_err_t post_display_command(httpd_req_t *req, const AwtrixCommand &command) {
    if (!awtrix_command_bus_post(command, 25)) {
        return AwtrixHttpServer::sendJson(req, "{\"ok\":false,\"error\":\"command queue full\"}", 503);
    }
    return HTTP_OK();
}

static bool build_http_command(httpd_req_t *req, const std::string &body, AwtrixCommand &command) {
    AwtrixProtocolError err;
    if (awtrix_protocol_http_command(req->uri, body.c_str(), command, err)) return true;
    const int code = err.code > 0 ? err.code : 400;
    std::string json = "{\"ok\":false,\"error\":\"" + err.message + "\"}";
    AwtrixHttpServer::sendJson(req, json.c_str(), code);
    return false;
}

static bool read_limited_body(httpd_req_t *req, std::string &body, int maxBytes = kMaxJsonBodyBytes) {
    if (req->content_len > maxBytes) {
        AwtrixHttpServer::sendJson(req, "{\"ok\":false,\"error\":\"body too large\"}", 413);
        return false;
    }
    body = AwtrixHttpServer::getBody(req);
    return true;
}

static bool normalize_spiffs_path(const char *input, char *out, size_t outSize) {
    if (!input || !*input || !out || outSize == 0) return false;
    while (*input == '/') input++;
    const size_t len = strlen(input);
    if (len == 0 || len > kMaxSpiffsRelativePath) return false;
    if (strstr(input, "..") || strchr(input, '\\') || strchr(input, ':')) return false;
    for (const char *p = input; *p; ++p) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 0x20 || ch == 0x7f) return false;
    }
    return snprintf(out, outSize, "/spiffs/%s", input) > 0;
}

static bool has_allowed_upload_extension(const char *path) {
    const char *dot = path ? strrchr(path, '.') : nullptr;
    if (!dot) return false;
    static const char *allowed[] = {".json", ".txt", ".gif", ".bin", ".bmp", ".jpg", ".jpeg", ".html", ".css", ".js"};
    for (const char *ext : allowed) {
        if (strcasecmp(dot, ext) == 0) return true;
    }
    return false;
}

static bool body_is_json(httpd_req_t *req, const std::string &body, bool objectOnly = true) {
    if (!objectOnly && body.empty()) return true;
    AwtrixProtocolError err;
    if (awtrix_protocol_validate_http_body(req->uri, body.c_str(), err)) return true;
    std::string json = "{\"ok\":false,\"error\":\"" + err.message + "\"}";
    AwtrixHttpServer::sendJson(req, json.c_str(), err.code > 0 ? err.code : 400);
    return false;
}

static bool constant_time_equal(const std::string &a, const char *b) {
    if (!b) return false;
    const size_t blen = strlen(b);
    const size_t maxLen = a.size() > blen ? a.size() : blen;
    unsigned char diff = (unsigned char)(a.size() ^ blen);
    for (size_t i = 0; i < maxLen; ++i) {
        const unsigned char ca = i < a.size() ? (unsigned char)a[i] : 0;
        const unsigned char cb = i < blen ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

/* ── Register all routes ──────────────────────────────────────── */
/* E3 (round 8): the per-route boilerplate is generated from
 * awtrix_routes.def using an X-macro pattern. The same .def file feeds
 * both the forward declarations above and the route count log below,
 * so all three stay in sync no matter how the table changes. */

/* Compile-time route count = number of ROUTE() rows in awtrix_routes.def. */
#define ROUTE(method, path, fn, auth) +1
constexpr int kRouteCount = 0
#include "awtrix_routes.def"
;
#undef ROUTE

/* Per-method dispatcher used by the registration X-macro below. We need
 * one helper per HTTP verb because AwtrixHttpServer exposes verb-specific
 * `onGet` / `onPost` methods (it doesn't take the verb as a runtime arg).
 * The functions are inline so the compiler folds them into the caller. */
namespace {
inline void route_register(AwtrixHttpServer &srv, const char *path,
                            AwtrixHttpServer::Handler fn, int /*get*/, bool /*auth*/) {
    srv.onGet(path, fn);
}
inline void route_register(AwtrixHttpServer &srv, const char *path,
                            AwtrixHttpServer::Handler fn, char /*post*/, bool /*auth*/) {
    srv.onPost(path, fn);
}
} // namespace

void awtrix_api_register_routes(AwtrixHttpServer &srv) {
    /* Special: root path is a front-door redirect, not a regular route. */
    srv.onGet("/", h_root);

    /* All other routes come from awtrix_routes.def. Adding a row there
     * automatically wires it up here and bumps the count in the log
     * line at the bottom of this function. */
    #define ROUTE(method, path, fn, auth) \
        route_register(srv, path, fn, method, auth);
    /* The `GET` / `POST` tokens at row-start are used as overload
     * discriminators in route_register() (int vs char) — see the two
     * inline helpers above. */
    #define GET  1
    #define POST 'P'
    #include "awtrix_routes.def"
    #undef GET
    #undef POST
    #undef ROUTE

    ESP_LOGI(TAG, "API routes registered (%d endpoints)", kRouteCount + 1);
}

/* ── Handler implementations ─────────────────────────────────── */

static esp_err_t h_version(httpd_req_t *req) {
    return AwtrixHttpServer::sendText(req, AWTRIX_VERSION);
}

static esp_err_t h_power(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body;
    if (!read_limited_body(req, body)) return ESP_OK;
    if (!body_is_json(req, body)) return ESP_OK;
    AwtrixCommand event;
    if (!build_http_command(req, body, event)) return ESP_OK;
    return post_display_command(req, event);
}

static esp_err_t h_sleep(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body;
    if (!read_limited_body(req, body)) return ESP_OK;
    if (!body_is_json(req, body)) return ESP_OK;
    AwtrixCommand event;
    if (!build_http_command(req, body, event)) return ESP_OK;
    return post_display_command(req, event);
}

static esp_err_t h_notify(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body;
    if (!read_limited_body(req, body)) return ESP_OK;
    if (body.empty()) return HTTP_OK();
    if (!body_is_json(req, body)) return ESP_OK;
    AwtrixCommand event;
    if (!build_http_command(req, body, event)) return ESP_OK;
    return post_display_command(req, event);
}

static esp_err_t h_dismiss(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    AwtrixCommand event;
    if (!build_http_command(req, "", event)) return ESP_OK;
    return post_display_command(req, event);
}

static esp_err_t h_apps_get(httpd_req_t *req) {
    return AwtrixHttpServer::sendJson(req, awtrix_query_apps_json().c_str());
}

static esp_err_t h_apps_post(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body;
    if (!read_limited_body(req, body)) return ESP_OK;
    if (!body_is_json(req, body)) return ESP_OK;
    AwtrixCommand event;
    if (!build_http_command(req, body, event)) return ESP_OK;
    return post_display_command(req, event);
}

static esp_err_t h_switch_app(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body;
    if (!read_limited_body(req, body)) return ESP_OK;
    if (!body_is_json(req, body)) return ESP_OK;
    AwtrixCommand event;
    if (!build_http_command(req, body, event)) return ESP_OK;
    return post_display_command(req, event);
}

static esp_err_t h_nextapp(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    AwtrixCommand event;
    if (!build_http_command(req, "", event)) return ESP_OK;
    return post_display_command(req, event);
}

static esp_err_t h_previousapp(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    AwtrixCommand event;
    if (!build_http_command(req, "", event)) return ESP_OK;
    return post_display_command(req, event);
}

static esp_err_t h_settings_get(httpd_req_t *req) {
    return AwtrixHttpServer::sendJson(req, awtrix_query_settings_json().c_str());
}

static esp_err_t h_settings_post(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body;
    if (!read_limited_body(req, body)) return ESP_OK;
    if (body.empty()) return HTTP_OK();
    if (!body_is_json(req, body)) return ESP_OK;
    AwtrixCommand event;
    if (!build_http_command(req, body, event)) return ESP_OK;
    return post_display_command(req, event);
}

static esp_err_t h_loop(httpd_req_t *req) {
    return AwtrixHttpServer::sendJson(req, awtrix_query_apps_with_icon_json().c_str());
}

static esp_err_t h_effects(httpd_req_t *req) {
    return AwtrixHttpServer::sendJson(req, awtrix_query_effect_names_json().c_str());
}

static esp_err_t h_transitions(httpd_req_t *req) {
    return AwtrixHttpServer::sendJson(req, awtrix_query_transition_names_json().c_str());
}

static esp_err_t h_reboot(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    HTTP_OK();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

/* ── Sound / RTTTL / R2D2 — connected to PeripheryManager ────── */
static esp_err_t h_rtttl(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body;
    if (!read_limited_body(req, body, 2048)) return ESP_OK;
    if (body.empty()) return AwtrixHttpServer::sendStatus(req, 400);
    bool ok = PeripheryManager::get().playRTTTL(body.c_str());
    return ok ? HTTP_OK() : AwtrixHttpServer::sendStatus(req, 500);
}

static esp_err_t h_sound(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body;
    if (!read_limited_body(req, body)) return ESP_OK;
    if (body.empty()) return AwtrixHttpServer::sendStatus(req, 400);
    if (!body_is_json(req, body)) return ESP_OK;
    bool ok = PeripheryManager::get().parseSound(body.c_str());
    return ok ? HTTP_OK() : AwtrixHttpServer::sendStatus(req, 500);
}

static esp_err_t h_r2d2(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body;
    if (!read_limited_body(req, body, 2048)) return ESP_OK;
    if (body.empty()) return AwtrixHttpServer::sendStatus(req, 400);
    PeripheryManager::get().r2d2(body.c_str());
    return HTTP_OK();
}

/* ── Moodlight — connected to DisplayManager ───────────────────── */
static esp_err_t h_moodlight(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body;
    if (!read_limited_body(req, body)) return ESP_OK;
    if (!body_is_json(req, body)) return ESP_OK;
    AwtrixCommand event;
    if (!build_http_command(req, body, event)) return ESP_OK;
    return post_display_command(req, event);
}

static esp_err_t h_erase(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    CONFIG.eraseAll();
    HTTP_OK();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_reset(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    CONFIG.eraseAll();
    HTTP_OK();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_reorder(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body;
    if (!read_limited_body(req, body)) return ESP_OK;
    if (!body_is_json(req, body)) return ESP_OK;
    AwtrixCommand event;
    if (!build_http_command(req, body, event)) return ESP_OK;
    return post_display_command(req, event);
}

static esp_err_t h_custom(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    /* Body shape: {"name":"foo","payload":{...}} (mirrors original API).
     * Either form works: caller can also send {"name":"foo", ...} where
     * the rest is the payload. We forward the entire body to parseCustomPage. */
    std::string body;
    if (!read_limited_body(req, body)) return ESP_OK;
    if (!body_is_json(req, body)) return ESP_OK;
    /* Extract the page name from JSON if provided */
    std::string name = "default";
    auto p = body.find("\"name\"");
    if (p != std::string::npos) {
        auto q = body.find('"', p + 6);
        if (q != std::string::npos) {
            auto r = body.find('"', q + 1);
            if (r != std::string::npos) name = body.substr(q + 1, r - q - 1);
        }
    }
    AwtrixCommand event;
    if (!build_http_command(req, body, event)) return ESP_OK;
    event.name = name;
    return post_display_command(req, event);
}

static esp_err_t h_stats(httpd_req_t *req) {
    return AwtrixHttpServer::sendJson(req, awtrix_query_stats_json().c_str());
}

static esp_err_t h_screen(httpd_req_t *req) {
    return AwtrixHttpServer::sendJson(req, DISPLAY_SNAPSHOT.screenJson().c_str());
}

static esp_err_t h_indicator(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body;
    if (!read_limited_body(req, body)) return ESP_OK;
    if (!body_is_json(req, body)) return ESP_OK;
    AwtrixCommand event;
    if (!build_http_command(req, body, event)) return ESP_OK;
    return post_display_command(req, event);
}

static esp_err_t h_save(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    CONFIG.save();
    return HTTP_OK();
}

/* ── Pack M+: AP-mode setup wizard endpoints ──────────────────────
 * Port of the original esp-fs-webserver /setup + /scan + /connect path.
 * The wizard HTML itself is the verbatim gzip blob from upstream so the
 * JS, CSS, RSSI bars and form layout look identical to the Arduino build.
 */

static esp_err_t h_setup(httpd_req_t *req) {
    return AwtrixHttpServer::sendGzipHtml(req, SETUP_HTML, SETUP_HTML_SIZE);
}

/* Simple percent-decode for application/x-www-form-urlencoded fields.
 * Decodes %XX hex sequences and '+' → ' '. Writes into `dst` (up to dstSize-1
 * chars, always NUL-terminated). Returns the number of bytes written. */
static size_t url_decode(const char *src, size_t srcLen, char *dst, size_t dstSize) {
    if (!dst || dstSize == 0) return 0;
    size_t o = 0;
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (size_t i = 0; i < srcLen && o + 1 < dstSize; ++i) {
        char ch = src[i];
        if (ch == '+') {
            dst[o++] = ' ';
        } else if (ch == '%' && i + 2 < srcLen) {
            int hi = hex_val(src[i + 1]);
            int lo = hex_val(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[o++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                dst[o++] = ch;
            }
        } else {
            dst[o++] = ch;
        }
    }
    dst[o] = '\0';
    return o;
}

/* Extract the value of `name=` from a form-urlencoded body. Returns true and
 * fills `out` (NUL-terminated, decoded) when found. */
static bool form_get_field(const std::string &body, const char *name, char *out, size_t outSize) {
    if (!name || !*name || !out || outSize == 0) return false;
    const size_t nameLen = strlen(name);
    size_t i = 0;
    while (i < body.size()) {
        /* find next '=' to delimit key */
        size_t eq = body.find('=', i);
        if (eq == std::string::npos) break;
        size_t amp = body.find('&', eq);
        if (amp == std::string::npos) amp = body.size();
        const size_t keyLen = eq - i;
        if (keyLen == nameLen && body.compare(i, nameLen, name) == 0) {
            url_decode(body.data() + eq + 1, amp - eq - 1, out, outSize);
            return true;
        }
        i = amp + 1;
    }
    out[0] = '\0';
    return false;
}

static esp_err_t h_scan(httpd_req_t *req) {
    /* Synchronous WiFi scan. The original ServerManager-based path used
     * Arduino's WiFi.scanNetworks() which blocks for the same ~2 seconds. */
    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    if (esp_wifi_scan_start(&cfg, /*block=*/true) != ESP_OK) {
        return AwtrixHttpServer::sendJson(req, "[]", 200);
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return AwtrixHttpServer::sendJson(req, "[]", 200);
    }
    if (found > 32) found = 32;     /* cap response size */
    wifi_ap_record_t *records = (wifi_ap_record_t *)calloc(found, sizeof(wifi_ap_record_t));
    if (!records) {
        return AwtrixHttpServer::sendJson(req, "[]", 200);
    }
    if (esp_wifi_scan_get_ap_records(&found, records) != ESP_OK) {
        free(records);
        return AwtrixHttpServer::sendJson(req, "[]", 200);
    }

    /* Build JSON: [{"ssid":"X","strength":"-50","security":true,"selected":false},...]
     * Mirrors the exact field names emitted by FSWebServer::handleScanNetworks so
     * the unmodified setup_htm.h consumes it correctly. */
    std::string out;
    out.reserve(found * 80 + 4);
    out += '[';
    for (uint16_t i = 0; i < found; ++i) {
        if (i) out += ',';
        out += "{\"ssid\":\"";
        /* very small json escape — ssid 32 bytes, control chars unlikely */
        for (const uint8_t *p = records[i].ssid; *p && p - records[i].ssid < 32; ++p) {
            char c = (char)*p;
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "\",\"strength\":\"%d\",\"security\":%s,\"selected\":false}",
                 (int)records[i].rssi,
                 (records[i].authmode == WIFI_AUTH_OPEN ? "false" : "true"));
        out += buf;
    }
    out += ']';
    free(records);
    return AwtrixHttpServer::sendJson(req, out.c_str(), 200);
}

/* Background task: tiny delay then esp_restart so the HTTP response gets
 * to the client before the radio drops. */
static void connect_restart_task(void *) {
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static esp_err_t h_connect(httpd_req_t *req) {
    /* Read the body (form-urlencoded). Cap at 1 KB — credentials are tiny.
     * Bug 4: check Content-Length BEFORE allocating; previous code allocated
     * first then rejected, leaving a DoS vector open. */
    if (req->content_len > 1024) {
        return AwtrixHttpServer::sendText(req, "Body too large", 413);
    }
    std::string body = AwtrixHttpServer::getBody(req);

    char ssid[33] = {0};      /* IEEE 802.11 SSID max is 32 bytes + NUL */
    char pass[65] = {0};      /* WPA passphrase max 63 + NUL, room for one extra */
    if (!form_get_field(body, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        return AwtrixHttpServer::sendText(req, "Missing ssid", 400);
    }
    /* password may be empty for open networks */
    form_get_field(body, "password", pass, sizeof(pass));

    /* Persist into NVS under the exact keys awtrix_network.cpp reads at boot
     * (WSSID / WPASS). These are deliberately *not* AwtrixConfig fields —
     * they live as plain NVS strings so a power-fail mid-save can't leave
     * the rest of the 70-field config in a half-written state. */
    awtrix_settings_save_str("WSSID", ssid);
    awtrix_settings_save_str("WPASS", pass);
    ESP_LOGI(TAG, "WiFi credentials saved (SSID=%s, %u-byte password)",
             ssid, (unsigned)strlen(pass));

    /* Tell the user we got it, then schedule a restart 800 ms later so the
     * radio doesn't drop mid-send. */
    AwtrixHttpServer::sendText(req, "Saved, restarting...", 200);
    xTaskCreate(connect_restart_task, "wifi_restart", 2048, nullptr, tskIDLE_PRIORITY + 1, nullptr);
    return ESP_OK;
}

/* P1-E: SPIFFS file editor / browser HTML.
 * The page is the verbatim gzip blob from upstream esp-fs-webserver; the
 * embedded JS calls the existing /list, /edit, /upload, /delete and /save
 * endpoints we already register. Auth-gated so an exposed device doesn't
 * leak its filesystem to unauthenticated visitors. */
static esp_err_t h_files_html(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    return AwtrixHttpServer::sendGzipHtml(req, EDIT_HTML, sizeof(EDIT_HTML));
}

/* ── Fullscreen WebUI: live pixel viewer in canvas ─────────────── */
static esp_err_t h_root(httpd_req_t *req) {
    /* Pack M+: in AP fallback the user has just joined the ad-hoc network
     * and likely visits "/" first; surface the setup wizard immediately
     * instead of showing a dead nav page. Mirrors esp-fs-webserver's
     * handleIndex fallback to handleSetup. */
    bool apMode;
    {
        AwtrixConfig::Guard guard(CONFIG);
        apMode = CONFIG.ap_mode;
    }
    if (apMode) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/setup");
        httpd_resp_send(req, nullptr, 0);
        return ESP_OK;
    }

    static const char HTML[] =
        "<!DOCTYPE html><html><head><meta charset='utf-8'><title>AWTRIX</title>"
        "<style>body{font-family:system-ui;background:#111;color:#eee;margin:0;padding:18px}"
        "h1{font-size:1.4em} a{color:#3af;display:inline-block;margin:6px 12px 6px 0}"
        ".card{background:#1d1d1d;padding:12px 16px;border-radius:6px;margin:10px 0}</style></head>"
        "<body><h1>AWTRIX 3 (ESP-IDF Port)</h1>"
        "<div class='card'>"
        "<a href='/fullscreen'>Live screen</a>"
        "<a href='/api/stats'>Stats</a>"
        "<a href='/api/settings'>Settings</a>"
        "<a href='/api/apps'>Apps</a>"
        "<a href='/api/effects'>Effects</a>"
        "<a href='/api/transitions'>Transitions</a>"
        "<a href='/list'>Files</a>"
        "<a href='/backup'>Backup</a>"
        "<a href='/version'>Version</a>"
        "</div>"
        "<p>Use the AWTRIX REST API or MQTT for full control.</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML, sizeof(HTML) - 1);
    return ESP_OK;
}

static esp_err_t h_screen_html(httpd_req_t *req) {
    /* Pack M: compact embedded-iframe screen viewer ported from the
     * original src/htmls.h::screen_html. Smaller pixel-scale (5x) and
     * no chrome — meant to live inside a third-party dashboard. The
     * sibling /api/screen returns raw JSON; this one is the HTML wrapper. */
    static const char HTML[] =
        "<!DOCTYPE html><html><head><meta charset='utf-8'><title>AWTRIX</title>"
        "<style>html,body{background:#000;margin:0;padding:0}"
        "canvas{image-rendering:pixelated;display:block;margin:0 auto}</style></head>"
        "<body><canvas id='cv' width='160' height='40'></canvas>"
        "<script>"
        "const cv=document.getElementById('cv'),cx=cv.getContext('2d');"
        "async function tick(){"
        "  try{const j=await(await fetch('/api/screen')).json();"
        "  for(let i=0;i<j.length;i++){const x=i%32,y=(i/32)|0;"
        "  cx.fillStyle='rgb('+(j[i]>>16&255)+','+(j[i]>>8&255)+','+(j[i]&255)+')';"
        "  cx.fillRect(x*5,y*5,5,5);}}catch(e){}"
        "  setTimeout(tick,200);}tick();</script></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML, sizeof(HTML) - 1);
    return ESP_OK;
}

static esp_err_t h_fullscreen(httpd_req_t *req) {
    static const char HTML[] =
        "<!DOCTYPE html><html><head><meta charset='utf-8'><title>AWTRIX screen</title>"
        "<style>body{background:#000;color:#aaa;font-family:system-ui;text-align:center;margin:0;padding:20px}"
        "canvas{image-rendering:pixelated;background:#111;border:1px solid #333}</style></head>"
        "<body><h2>AWTRIX live screen</h2>"
        "<canvas id='cv' width='320' height='80'></canvas>"
        "<script>"
        "const cv=document.getElementById('cv'),cx=cv.getContext('2d');"
        "async function tick(){"
        "  try{const j=await(await fetch('/api/screen')).json();"
        "  for(let i=0;i<j.length;i++){const x=i%32,y=(i/32)|0;"
        "  cx.fillStyle='rgb('+(j[i]>>16&255)+','+(j[i]>>8&255)+','+(j[i]&255)+')';"
        "  cx.fillRect(x*10,y*10,10,10);}}catch(e){}"
        "  setTimeout(tick,200);}tick();</script></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML, sizeof(HTML) - 1);
    return ESP_OK;
}

static esp_err_t h_backup(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=awtrix_backup.json");
    return AwtrixHttpServer::sendJson(req, awtrix_query_settings_json().c_str());
}

/* ── SPIFFS browser (port of lib/webserver list/upload/delete) ── */
static esp_err_t h_list(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    DIR *d = opendir("/spiffs");
    if (!d) return AwtrixHttpServer::sendJson(req, "[]");
    std::string out = "[";
    bool first = true;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        struct stat st;
        std::string full = std::string("/spiffs/") + e->d_name;
        if (stat(full.c_str(), &st) != 0) continue;
        if (!first) out += ',';
        first = false;
        out += "{\"name\":\""; out += e->d_name;
        out += "\",\"size\":";
        char sz[16]; snprintf(sz, sizeof(sz), "%lld", (long long)st.st_size);
        out += sz; out += "}";
    }
    closedir(d);
    out += "]";
    return AwtrixHttpServer::sendJson(req, out.c_str());
}

static esp_err_t h_upload(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    if (req->content_len > kMaxUploadBytes) {
        return AwtrixHttpServer::sendStatus(req, 413);
    }

    /* Body is the raw file bytes; query-string carries ?path=/foo.bin
     * (or any "Content-Filename" hdr). This is intentionally simpler than
     * full multipart parsing — sufficient for the AWTRIX WebUI uploader. */
    char path[128] = "/spiffs/upload.bin";
    char qbuf[128];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[96];
        if (httpd_query_key_value(qbuf, "path", val, sizeof(val)) == ESP_OK) {
            if (!normalize_spiffs_path(val, path, sizeof(path))) return AwtrixHttpServer::sendStatus(req, 400);
        }
    }
    if (!has_allowed_upload_extension(path)) return AwtrixHttpServer::sendStatus(req, 415);
    FILE *fp = fopen(path, "wb");
    if (!fp) return AwtrixHttpServer::sendStatus(req, 500);
    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, sizeof(buf) < (size_t)remaining ? sizeof(buf) : (size_t)remaining);
        if (n <= 0) { fclose(fp); remove(path); return AwtrixHttpServer::sendStatus(req, 500); }
        fwrite(buf, 1, n, fp);
        remaining -= n;
    }
    fclose(fp);
    ESP_LOGI(TAG, "Uploaded %d bytes -> %s", req->content_len, path);
    return HTTP_OK();
}

static esp_err_t h_delete(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body;
    if (!read_limited_body(req, body, 256)) return ESP_OK;
    if (body.empty()) return AwtrixHttpServer::sendStatus(req, 400);
    char full[128];
    if (!normalize_spiffs_path(body.c_str(), full, sizeof(full))) return AwtrixHttpServer::sendStatus(req, 400);
    if (remove(full) != 0) return AwtrixHttpServer::sendStatus(req, 404);
    return HTTP_OK();
}

static esp_err_t h_edit(httpd_req_t *req) {
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    char qbuf[128];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK)
        return AwtrixHttpServer::sendStatus(req, 400);
    char val[96];
    if (httpd_query_key_value(qbuf, "file", val, sizeof(val)) != ESP_OK)
        return AwtrixHttpServer::sendStatus(req, 400);
    char path[128];
    if (!normalize_spiffs_path(val, path, sizeof(path))) return AwtrixHttpServer::sendStatus(req, 400);
    FILE *fp = fopen(path, "rb");
    if (!fp) return AwtrixHttpServer::sendStatus(req, 404);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    if (sz < 0 || sz > 64 * 1024) { fclose(fp); return AwtrixHttpServer::sendStatus(req, 413); }
    char *buf = (char *)calloc(sz + 1, 1);   /* Bug 5: zero-init prevents
                                               stale heap leak if fread
                                               under-reads. */
    if (!buf) { fclose(fp); return AwtrixHttpServer::sendStatus(req, 500); }
    size_t got = fread(buf, 1, sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        /* Bug 5: short read = either a filesystem error or a race with
         * another writer truncating the file. Either way, we cannot
         * trust the buffer's tail bytes. Refuse rather than ship
         * possibly-corrupted content. The calloc above already zeroed
         * the unread region so even an attacker triggering this path
         * gets nothing useful, but the explicit refusal is the right
         * semantic. */
        free(buf);
        return AwtrixHttpServer::sendStatus(req, 500);
    }
    buf[sz] = '\0';
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, sz);
    free(buf);
    return ESP_OK;
}

/* ── BUTTON_CALLBACK ─────────────────────────────────────────────
 * Fires the user-configured webhook (CONFIG.buttonCallback) whenever a
 * physical button is pressed. The original AWTRIX3 used a synchronous
 * HTTPClient.GET() call; we run the actual request on a one-shot
 * FreeRTOS task so the button event handler stays non-blocking. */

/* ── BUTTON_CALLBACK worker ──────────────────────────────────────
 * The original implementation spawned a fresh `xTaskCreate("btncb", 4096, …)`
 * for every button press. Two real failure modes:
 *   1. Spam (long-press repeats, double-tap) piles up several short-lived
 *      tasks simultaneously — bursts of 20 KB+ stack consumption.
 *   2. esp_http_client wants ~3.5 KB stack for plain HTTP and >5 KB for
 *      HTTPS; 4 KB is fragile and silently corrupts on overflow.
 *
 * New design: a single long-running worker task (lazily created on first
 * use) drains a fixed-size URL queue. Producers push with zero-tick
 * timeout — under flood we drop the oldest event and log, instead of
 * blocking the button event handler. The worker has a 6144-byte stack so
 * https + the 256-byte URL buffer fit comfortably.
 *
 * The lazy-init lock stays a portMUX (no FreeRTOS mutex needed) because
 * the only contention is the first few microseconds of two near-
 * simultaneous presses racing to create the worker.
 */

#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define BTNCB_URL_LEN     256
#define BTNCB_QUEUE_DEPTH 4
#define BTNCB_TASK_STACK  6144
#define BTNCB_TASK_PRIO   4

struct btn_cb_msg_t { char url[BTNCB_URL_LEN]; };

static QueueHandle_t s_btncb_queue   = nullptr;
static TaskHandle_t  s_btncb_worker  = nullptr;
static portMUX_TYPE  s_btncb_initLock = portMUX_INITIALIZER_UNLOCKED;

static void btn_cb_worker_task(void *) {
    btn_cb_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_btncb_queue, &msg, portMAX_DELAY) != pdTRUE) continue;
        esp_http_client_config_t cfg = {};
        cfg.url        = msg.url;
        cfg.timeout_ms = 4000;
        cfg.method     = HTTP_METHOD_POST;
        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (cli) {
            esp_http_client_set_post_field(cli, "", 0);
            esp_err_t err = esp_http_client_perform(cli);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "btncb HTTP error: %s", esp_err_to_name(err));
            }
            esp_http_client_cleanup(cli);
        }
        /* Loop back for the next queued URL — task lives forever. */
    }
}

static bool btn_cb_lazy_init(void) {
    if (s_btncb_queue && s_btncb_worker) return true;
    portENTER_CRITICAL(&s_btncb_initLock);
    bool ok = (s_btncb_queue && s_btncb_worker);
    if (!ok) {
        if (!s_btncb_queue) {
            s_btncb_queue = xQueueCreate(BTNCB_QUEUE_DEPTH, sizeof(btn_cb_msg_t));
        }
        if (s_btncb_queue && !s_btncb_worker) {
            BaseType_t rc = xTaskCreate(btn_cb_worker_task, "btncb",
                                        BTNCB_TASK_STACK, NULL,
                                        BTNCB_TASK_PRIO, &s_btncb_worker);
            if (rc != pdPASS) s_btncb_worker = NULL;
        }
        ok = (s_btncb_queue && s_btncb_worker);
    }
    portEXIT_CRITICAL(&s_btncb_initLock);
    return ok;
}

extern "C" void awtrix_button_callback_fire(int idx, int evt) {
    if (CONFIG.buttonCallback.empty()) return;
    if (!btn_cb_lazy_init()) {
        ESP_LOGE(TAG, "btncb worker init failed (no heap?)");
        return;
    }
    /* New-style fields (round 7 / pack P keeps them as the primary protocol). */
    const char *btn = (idx == 0) ? "L" : (idx == 1) ? "S" : (idx == 2) ? "R" : "RST";
    const char *ev  = (evt == /*BTN_EVENT_LONG_PRESS*/3)      ? "long"
                    : (evt == /*BTN_EVENT_DOUBLE_PRESS*/2)    ? "double"
                    : (evt == /*BTN_EVENT_VERY_LONG_PRESS*/4) ? "verylong" : "press";

    /* Pack P: also emit the legacy Arduino protocol fields
     *   button=left|middle|right&state=0|1&uid=<deviceId>
     * so existing webhook receivers (written against AWTRIX3 v2) keep
     * working. RESET (idx=3) had no legacy counterpart — the original
     * firmware never reported it — so we skip the button= field for it. */
    const char *legacy_btn = (idx == 0) ? "left"
                           : (idx == 1) ? "middle"
                           : (idx == 2) ? "right"
                           : nullptr;
    /* In the original firmware state==1 was emitted on PRESS and state==0
     * on RELEASE; the new path only forwards discrete events, so map
     * every action-on event (press/long/double/verylong) to state=1. */
    const int   legacy_state = 1;

    btn_cb_msg_t msg{};
    const char *sep = strchr(CONFIG.buttonCallback.c_str(), '?') ? "&" : "?";
    int n;
    if (legacy_btn) {
        n = snprintf(msg.url, sizeof(msg.url),
                     "%s%sbtn=%s&evt=%s&button=%s&state=%d&uid=%s",
                     CONFIG.buttonCallback.c_str(), sep,
                     btn, ev, legacy_btn, legacy_state,
                     CONFIG.uniqueID.c_str());
    } else {
        n = snprintf(msg.url, sizeof(msg.url),
                     "%s%sbtn=%s&evt=%s&uid=%s",
                     CONFIG.buttonCallback.c_str(), sep,
                     btn, ev, CONFIG.uniqueID.c_str());
    }
    if (n <= 0 || n >= (int)sizeof(msg.url)) {
        ESP_LOGW(TAG, "btncb URL too long, dropped");
        return;
    }
    /* Zero-tick timeout: never block the button event path. If the queue
     * is already full (4 in flight) we drop and log — the user can still
     * tell from the log that a press wasn't delivered. */
    if (xQueueSend(s_btncb_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "btncb queue full, dropping URL=%s", msg.url);
    }
}

/* ── HTTP Basic auth gate ────────────────────────────────────────
 * Returns true when auth is disabled (no auth_user) or when the request's
 * Authorization header matches CONFIG.auth_user / auth_pass. On mismatch
 * sends 401 + WWW-Authenticate and returns false. The caller must `return ESP_OK`
 * after a false return to terminate the handler cleanly. */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const std::string &in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t v = (uint32_t)(uint8_t)in[i] << 16;
        if (i + 1 < in.size()) v |= (uint32_t)(uint8_t)in[i + 1] << 8;
        if (i + 2 < in.size()) v |= (uint32_t)(uint8_t)in[i + 2];
        out.push_back(b64_table[(v >> 18) & 0x3F]);
        out.push_back(b64_table[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < in.size() ? b64_table[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < in.size() ? b64_table[v & 0x3F]        : '=');
    }
    return out;
}

bool awtrix_http_require_auth(httpd_req_t *req) {
    std::string authUser;
    std::string authPass;
    {
        AwtrixConfig::Guard guard(CONFIG);
        authUser = CONFIG.auth_user;
        authPass = CONFIG.auth_pass;
    }
    if (authUser.empty()) return true;             /* auth disabled */

    /* Fetch the Authorization header. */
    size_t hlen = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hlen > 0 && hlen < 256) {
        char hdr[256];
        if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK) {
            const char *prefix = "Basic ";
            if (strncmp(hdr, prefix, strlen(prefix)) == 0) {
                std::string expected = base64_encode(authUser + ":" + authPass);
                if (constant_time_equal(expected, hdr + strlen(prefix))) return true;
            }
        }
    }

    /* Reject with 401 + WWW-Authenticate so the browser shows a login prompt. */
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"AWTRIX\"");
    httpd_resp_send(req, "Unauthorized", 12);
    return false;
}
