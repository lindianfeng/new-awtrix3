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
#include "DisplayManager.h"
#include "awtrix_periphery.h"
#include "awtrix_power.h"
#include "awtrix_games.h"
#include <esp_log.h>
#include <esp_spiffs.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = TAG_HTTP;

/* ── Forward declarations for all route handlers ─────────────── */
static esp_err_t h_version(httpd_req_t * req);
static esp_err_t h_power(httpd_req_t * req);
static esp_err_t h_sleep(httpd_req_t * req);
static esp_err_t h_notify(httpd_req_t * req);
static esp_err_t h_dismiss(httpd_req_t * req);
static esp_err_t h_apps_get(httpd_req_t * req);
static esp_err_t h_apps_post(httpd_req_t * req);
static esp_err_t h_switch_app(httpd_req_t * req);
static esp_err_t h_nextapp(httpd_req_t * req);
static esp_err_t h_previousapp(httpd_req_t * req);
static esp_err_t h_settings_get(httpd_req_t * req);
static esp_err_t h_settings_post(httpd_req_t * req);
static esp_err_t h_loop(httpd_req_t * req);
static esp_err_t h_effects(httpd_req_t * req);
static esp_err_t h_transitions(httpd_req_t * req);
static esp_err_t h_reboot(httpd_req_t * req);
static esp_err_t h_rtttl(httpd_req_t * req);
static esp_err_t h_sound(httpd_req_t * req);
static esp_err_t h_moodlight(httpd_req_t * req);
static esp_err_t h_erase(httpd_req_t * req);
static esp_err_t h_reset(httpd_req_t * req);
static esp_err_t h_reorder(httpd_req_t * req);
static esp_err_t h_custom(httpd_req_t * req);
static esp_err_t h_stats(httpd_req_t * req);
static esp_err_t h_screen(httpd_req_t * req);
static esp_err_t h_indicator(httpd_req_t * req);
static esp_err_t h_r2d2(httpd_req_t * req);
static esp_err_t h_save(httpd_req_t * req);
static esp_err_t h_game_says(httpd_req_t * req);
static esp_err_t h_game_slot(httpd_req_t * req);
static esp_err_t h_fullscreen(httpd_req_t * req);
static esp_err_t h_backup(httpd_req_t * req);
static esp_err_t h_list(httpd_req_t * req);
static esp_err_t h_upload(httpd_req_t * req);
static esp_err_t h_delete(httpd_req_t * req);
static esp_err_t h_edit(httpd_req_t * req);
static esp_err_t h_root(httpd_req_t * req);

/* HTTP Basic-auth gate (defined at end of file). Returns false and sends
 * 401+WWW-Authenticate when CONFIG.auth_user is set and the request fails
 * authentication. Sensitive POST handlers call this first. */
bool awtrix_http_require_auth(httpd_req_t * req);

/* ── Register all routes ──────────────────────────────────────── */
void awtrix_api_register_routes(AwtrixHttpServer& srv)
{
    srv.onGet("/", h_root);
    srv.onGet("/version", h_version);
    srv.onGet("/api/loop", h_loop);
    srv.onGet("/api/effects", h_effects);
    srv.onGet("/api/transitions", h_transitions);
    srv.onGet("/api/apps", h_apps_get);
    srv.onGet("/api/settings", h_settings_get);
    srv.onGet("/api/stats", h_stats);
    srv.onGet("/api/screen", h_screen);
    srv.onGet("/fullscreen", h_fullscreen);
    srv.onGet("/backup", h_backup);
    srv.onGet("/list", h_list);
    srv.onGet("/edit", h_edit);

    srv.onPost("/api/power", h_power);
    srv.onPost("/api/sleep", h_sleep);
    srv.onPost("/api/notify", h_notify);
    srv.onPost("/api/notify/dismiss", h_dismiss);
    srv.onPost("/api/apps", h_apps_post);
    srv.onPost("/api/switch", h_switch_app);
    srv.onPost("/api/nextapp", h_nextapp);
    srv.onPost("/api/previousapp", h_previousapp);
    srv.onPost("/api/settings", h_settings_post);
    srv.onPost("/api/reboot", h_reboot);
    srv.onPost("/api/rtttl", h_rtttl);
    srv.onPost("/api/sound", h_sound);
    srv.onPost("/api/moodlight", h_moodlight);
    srv.onPost("/api/erase", h_erase);
    srv.onPost("/api/resetSettings", h_reset);
    srv.onPost("/api/reorder", h_reorder);
    srv.onPost("/api/custom", h_custom);
    srv.onPost("/api/indicator1", h_indicator);
    srv.onPost("/api/indicator2", h_indicator);
    srv.onPost("/api/indicator3", h_indicator);
    srv.onPost("/api/r2d2", h_r2d2);
    srv.onPost("/api/game/says", h_game_says);
    srv.onPost("/api/game/slot", h_game_slot);
    srv.onPost("/upload", h_upload);
    srv.onPost("/delete", h_delete);
    srv.onPost("/save", h_save);

    ESP_LOGI(TAG, "API routes registered (37 endpoints)");
}

/* ── Handler implementations ─────────────────────────────────── */

static esp_err_t h_version(httpd_req_t* req)
{
    return AwtrixHttpServer::sendText(req, AWTRIX_VERSION);
}

static esp_err_t h_power(httpd_req_t* req)
{
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body = AwtrixHttpServer::getBody(req);
    DisplayManager::get().powerStateParse(body.c_str());
    return HTTP_OK();
}

static esp_err_t h_sleep(httpd_req_t* req)
{
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body = AwtrixHttpServer::getBody(req);
    awtrix_power_sleep_parser(body.c_str());
    return HTTP_OK();
}

static esp_err_t h_notify(httpd_req_t* req)
{
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body = AwtrixHttpServer::getBody(req);
    if (!body.empty() && !DisplayManager::get().generateNotification(1, body.c_str()))
        return AwtrixHttpServer::sendStatus(req, 500);
    return HTTP_OK();
}

static esp_err_t h_dismiss(httpd_req_t* req)
{
    DisplayManager::get().dismissNotify();
    return HTTP_OK();
}

static esp_err_t h_apps_get(httpd_req_t* req)
{
    return AwtrixHttpServer::sendJson(req, DisplayManager::get().getAppsAsJson().c_str());
}

static esp_err_t h_apps_post(httpd_req_t* req)
{
    std::string body = AwtrixHttpServer::getBody(req);
    DisplayManager::get().updateAppVector(body.c_str());
    return HTTP_OK();
}

static esp_err_t h_switch_app(httpd_req_t* req)
{
    std::string body = AwtrixHttpServer::getBody(req);
    if (!DisplayManager::get().switchToApp(body.c_str()))
        return AwtrixHttpServer::sendStatus(req, 500);
    return HTTP_OK();
}

static esp_err_t h_nextapp(httpd_req_t* req)
{
    DisplayManager::get().nextApp();
    return HTTP_OK();
}

static esp_err_t h_previousapp(httpd_req_t* req)
{
    DisplayManager::get().previousApp();
    return HTTP_OK();
}

static esp_err_t h_settings_get(httpd_req_t* req)
{
    return AwtrixHttpServer::sendJson(req, DisplayManager::get().getSettings().c_str());
}

static esp_err_t h_settings_post(httpd_req_t* req)
{
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body = AwtrixHttpServer::getBody(req);
    if (!body.empty()) DisplayManager::get().setNewSettings(body.c_str());
    CONFIG.save();
    return HTTP_OK();
}

static esp_err_t h_loop(httpd_req_t* req)
{
    return AwtrixHttpServer::sendJson(req, DisplayManager::get().getAppsWithIcon().c_str());
}

static esp_err_t h_effects(httpd_req_t* req)
{
    return AwtrixHttpServer::sendJson(req, DisplayManager::get().getEffectNames().c_str());
}

static esp_err_t h_transitions(httpd_req_t* req)
{
    return AwtrixHttpServer::sendJson(req, DisplayManager::get().getTransitionNames().c_str());
}

static esp_err_t h_reboot(httpd_req_t* req)
{
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    HTTP_OK();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

/* ── Sound / RTTTL / R2D2 — connected to PeripheryManager ────── */
static esp_err_t h_rtttl(httpd_req_t* req)
{
    std::string body = AwtrixHttpServer::getBody(req);
    if (body.empty()) return AwtrixHttpServer::sendStatus(req, 400);
    bool ok = PeripheryManager::get().playRTTTL(body.c_str());
    return ok ? HTTP_OK() : AwtrixHttpServer::sendStatus(req, 500);
}

static esp_err_t h_sound(httpd_req_t* req)
{
    std::string body = AwtrixHttpServer::getBody(req);
    if (body.empty()) return AwtrixHttpServer::sendStatus(req, 400);
    bool ok = PeripheryManager::get().parseSound(body.c_str());
    return ok ? HTTP_OK() : AwtrixHttpServer::sendStatus(req, 500);
}

static esp_err_t h_r2d2(httpd_req_t* req)
{
    std::string body = AwtrixHttpServer::getBody(req);
    if (body.empty()) return AwtrixHttpServer::sendStatus(req, 400);
    PeripheryManager::get().r2d2(body.c_str());
    return HTTP_OK();
}

/* ── Moodlight — connected to DisplayManager ───────────────────── */
static esp_err_t h_moodlight(httpd_req_t* req)
{
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body = AwtrixHttpServer::getBody(req);
    bool ok = DisplayManager::get().moodlight(body.c_str());
    return ok ? HTTP_OK() : AwtrixHttpServer::sendStatus(req, 400);
}

static esp_err_t h_erase(httpd_req_t* req)
{
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    CONFIG.eraseAll();
    HTTP_OK();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_reset(httpd_req_t* req)
{
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    CONFIG.eraseAll();
    HTTP_OK();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_reorder(httpd_req_t* req)
{
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    std::string body = AwtrixHttpServer::getBody(req);
    DisplayManager::get().reorderApps(body.c_str());
    return HTTP_OK();
}

static esp_err_t h_custom(httpd_req_t* req)
{
    if (!awtrix_http_require_auth(req)) return ESP_OK;
    /* Body shape: {"name":"foo","payload":{...}} (mirrors original API).
     * Either form works: caller can also send {"name":"foo", ...} where
     * the rest is the payload. We forward the entire body to parseCustomPage. */
    std::string body = AwtrixHttpServer::getBody(req);
    /* Extract the page name from JSON if provided */
    std::string name = "default";
    auto p = body.find("\"name\"");
    if (p != std::string::npos)
    {
        auto q = body.find('"', p + 6);
        if (q != std::string::npos)
        {
            auto r = body.find('"', q + 1);
            if (r != std::string::npos) name = body.substr(q + 1, r - q - 1);
        }
    }
    DisplayManager::get().parseCustomPage(name.c_str(), body.c_str(), false);
    return HTTP_OK();
}

static esp_err_t h_stats(httpd_req_t* req)
{
    return AwtrixHttpServer::sendJson(req, DisplayManager::get().getStats().c_str());
}

static esp_err_t h_screen(httpd_req_t* req)
{
    return AwtrixHttpServer::sendJson(req, DisplayManager::get().ledsAsJson().c_str());
}

static esp_err_t h_indicator(httpd_req_t* req)
{
    std::string body = AwtrixHttpServer::getBody(req);
    int indicator = 1;
    if (strcmp(req->uri, "/api/indicator2") == 0) indicator = 2;
    else if (strcmp(req->uri, "/api/indicator3") == 0) indicator = 3;
    DisplayManager::get().indicatorParser(indicator, body.c_str());
    return HTTP_OK();
}

static esp_err_t h_save(httpd_req_t* req)
{
    CONFIG.save();
    return HTTP_OK();
}

/* ── Games (new endpoints) ─────────────────────────────────────── */
static esp_err_t h_game_says(httpd_req_t* req)
{
    awtrix_game_awtrix_says_start();
    return HTTP_OK();
}

static esp_err_t h_game_slot(httpd_req_t* req)
{
    awtrix_game_slot_machine_start();
    return HTTP_OK();
}

/* ── Fullscreen WebUI: live pixel viewer in canvas ─────────────── */
static esp_err_t h_root(httpd_req_t* req)
{
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

static esp_err_t h_fullscreen(httpd_req_t* req)
{
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

static esp_err_t h_backup(httpd_req_t* req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=awtrix_backup.json");
    return AwtrixHttpServer::sendJson(req, DisplayManager::get().getSettings().c_str());
}

/* ── SPIFFS browser (port of lib/webserver list/upload/delete) ── */
static esp_err_t h_list(httpd_req_t* req)
{
    DIR* d = opendir("/spiffs");
    if (!d) return AwtrixHttpServer::sendJson(req, "[]");
    std::string out = "[";
    bool first = true;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr)
    {
        struct stat st;
        std::string full = std::string("/spiffs/") + e->d_name;
        if (stat(full.c_str(), &st) != 0) continue;
        if (!first) out += ',';
        first = false;
        out += "{\"name\":\"";
        out += e->d_name;
        out += "\",\"size\":";
        char sz[16];
        snprintf(sz, sizeof(sz), "%lld", (long long)st.st_size);
        out += sz;
        out += "}";
    }
    closedir(d);
    out += "]";
    return AwtrixHttpServer::sendJson(req, out.c_str());
}

static esp_err_t h_upload(httpd_req_t* req)
{
    /* Body is the raw file bytes; query-string carries ?path=/foo.bin
     * (or any "Content-Filename" hdr). This is intentionally simpler than
     * full multipart parsing — sufficient for the AWTRIX WebUI uploader. */
    char path[128] = "/spiffs/upload.bin";
    char qbuf[128];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK)
    {
        char val[64];
        if (httpd_query_key_value(qbuf, "path", val, sizeof(val)) == ESP_OK)
        {
            snprintf(path, sizeof(path), "/spiffs/%s", val[0] == '/' ? val + 1 : val);
        }
    }
    FILE* fp = fopen(path, "wb");
    if (!fp) return AwtrixHttpServer::sendStatus(req, 500);
    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0)
    {
        int n = httpd_req_recv(req, buf, sizeof(buf) < (size_t)remaining ? sizeof(buf) : (size_t)remaining);
        if (n <= 0)
        {
            fclose(fp);
            return AwtrixHttpServer::sendStatus(req, 500);
        }
        fwrite(buf, 1, n, fp);
        remaining -= n;
    }
    fclose(fp);
    ESP_LOGI(TAG, "Uploaded %d bytes -> %s", req->content_len, path);
    return HTTP_OK();
}

static esp_err_t h_delete(httpd_req_t* req)
{
    std::string body = AwtrixHttpServer::getBody(req);
    if (body.empty()) return AwtrixHttpServer::sendStatus(req, 400);
    /* Strip an optional "/" prefix. Body is the file basename. */
    const char* name = body.c_str();
    if (*name == '/') name++;
    char full[80];
    snprintf(full, sizeof(full), "/spiffs/%s", name);
    if (remove(full) != 0) return AwtrixHttpServer::sendStatus(req, 404);
    return HTTP_OK();
}

static esp_err_t h_edit(httpd_req_t* req)
{
    char qbuf[128];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK)
        return AwtrixHttpServer::sendStatus(req, 400);
    char val[64];
    if (httpd_query_key_value(qbuf, "file", val, sizeof(val)) != ESP_OK)
        return AwtrixHttpServer::sendStatus(req, 400);
    char path[80];
    snprintf(path, sizeof(path), "/spiffs/%s", val[0] == '/' ? val + 1 : val);
    FILE* fp = fopen(path, "rb");
    if (!fp) return AwtrixHttpServer::sendStatus(req, 404);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    if (sz < 0 || sz > 64 * 1024)
    {
        fclose(fp);
        return AwtrixHttpServer::sendStatus(req, 413);
    }
    char* buf = (char*)malloc(sz + 1);
    if (!buf)
    {
        fclose(fp);
        return AwtrixHttpServer::sendStatus(req, 500);
    }
    fread(buf, 1, sz, fp);
    buf[sz] = '\0';
    fclose(fp);
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

struct btn_cb_msg_t
{
    char url[BTNCB_URL_LEN];
};

static QueueHandle_t s_btncb_queue = nullptr;
static TaskHandle_t s_btncb_worker = nullptr;
static portMUX_TYPE s_btncb_initLock = portMUX_INITIALIZER_UNLOCKED;

static void btn_cb_worker_task(void*)
{
    btn_cb_msg_t msg;
    for (;;)
    {
        if (xQueueReceive(s_btncb_queue, &msg, portMAX_DELAY) != pdTRUE) continue;
        esp_http_client_config_t cfg = {};
        cfg.url = msg.url;
        cfg.timeout_ms = 4000;
        cfg.method = HTTP_METHOD_POST;
        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (cli)
        {
            esp_http_client_set_post_field(cli, "", 0);
            esp_err_t err = esp_http_client_perform(cli);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "btncb HTTP error: %s", esp_err_to_name(err));
            }
            esp_http_client_cleanup(cli);
        }
        /* Loop back for the next queued URL — task lives forever. */
    }
}

static bool btn_cb_lazy_init(void)
{
    if (s_btncb_queue && s_btncb_worker) return true;
    portENTER_CRITICAL(&s_btncb_initLock);
    bool ok = (s_btncb_queue && s_btncb_worker);
    if (!ok)
    {
        if (!s_btncb_queue)
        {
            s_btncb_queue = xQueueCreate(BTNCB_QUEUE_DEPTH, sizeof(btn_cb_msg_t));
        }
        if (s_btncb_queue && !s_btncb_worker)
        {
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

extern "C" void awtrix_button_callback_fire(int idx, int evt)
{
    if (CONFIG.buttonCallback.empty()) return;
    if (!btn_cb_lazy_init())
    {
        ESP_LOGE(TAG, "btncb worker init failed (no heap?)");
        return;
    }
    const char* btn = (idx == 0) ? "L" : (idx == 1) ? "S" : (idx == 2) ? "R" : "RST";
    const char* ev = (evt == /*BTN_EVENT_LONG_PRESS*/3)
                         ? "long"
                         : (evt == /*BTN_EVENT_DOUBLE_PRESS*/2)
                         ? "double"
                         : (evt == /*BTN_EVENT_VERY_LONG_PRESS*/4)
                         ? "verylong"
                         : "press";

    btn_cb_msg_t msg{};
    const char* sep = strchr(CONFIG.buttonCallback.c_str(), '?') ? "&" : "?";
    int n = snprintf(msg.url, sizeof(msg.url), "%s%sbtn=%s&evt=%s",
                     CONFIG.buttonCallback.c_str(), sep, btn, ev);
    if (n <= 0 || n >= (int)sizeof(msg.url))
    {
        ESP_LOGW(TAG, "btncb URL too long, dropped");
        return;
    }
    /* Zero-tick timeout: never block the button event path. If the queue
     * is already full (4 in flight) we drop and log — the user can still
     * tell from the log that a press wasn't delivered. */
    if (xQueueSend(s_btncb_queue, &msg, 0) != pdTRUE)
    {
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

static std::string base64_encode(const std::string& in)
{
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3)
    {
        uint32_t v = (uint32_t)(uint8_t)
        in[i] << 16;
        if (i + 1 < in.size()) v |= (uint32_t)(uint8_t)
        in[i + 1] << 8;
        if (i + 2 < in.size()) v |= (uint32_t)(uint8_t)
        in[i + 2];
        out.push_back(b64_table[(v >> 18) & 0x3F]);
        out.push_back(b64_table[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < in.size() ? b64_table[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < in.size() ? b64_table[v & 0x3F] : '=');
    }
    return out;
}

bool awtrix_http_require_auth(httpd_req_t* req)
{
    auto& c = CONFIG;
    if (c.auth_user.empty()) return true; /* auth disabled */

    /* Fetch the Authorization header. */
    size_t hlen = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hlen > 0 && hlen < 256)
    {
        char hdr[256];
        if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK)
        {
            const char* prefix = "Basic ";
            if (strncmp(hdr, prefix, strlen(prefix)) == 0)
            {
                std::string expected = base64_encode(c.auth_user + ":" + c.auth_pass);
                if (expected == (hdr + strlen(prefix))) return true;
            }
        }
    }

    /* Reject with 401 + WWW-Authenticate so the browser shows a login prompt. */
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"AWTRIX\"");
    httpd_resp_send(req, "Unauthorized", 12);
    return false;
}
