#include "awtrix_http.h"
#include "awtrix_hal.h"
#include "awtrix_globals.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <esp_log.h>
#include <esp_spiffs.h>

static const char* TAG = TAG_HTTP;

/* ── route descriptor ─────────────────────────────────────────── */
struct route_t
{
    std::string uri;
    httpd_method_t method;
    AwtrixHttpServer::Handler handler;
};

static std::vector<route_t> s_routes;

AwtrixHttpServer::AwtrixHttpServer()
{
}

AwtrixHttpServer::~AwtrixHttpServer() { stop(); }

/* ── global handler ───────────────────────────────────────────── */
esp_err_t AwtrixHttpServer::static_handler(httpd_req_t* req)
{
    for (auto& r : s_routes)
    {
        if (r.uri != req->uri) continue;
        if (static_cast<int>(r.method) != HTTP_ANY &&
            static_cast<int>(r.method) != req->method)
            continue;
        if (r.handler) return r.handler(req);
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

/* ── start / stop ─────────────────────────────────────────────── */
bool AwtrixHttpServer::start(uint16_t port)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 64;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&m_handle, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return false;
    }

    /* register catch-all */
    httpd_uri_t fallback = {};
    fallback.uri = "/*";
    fallback.method = static_cast<httpd_method_t>(HTTP_ANY);
    fallback.handler = static_handler;
    fallback.user_ctx = nullptr;
    httpd_register_uri_handler(m_handle, &fallback);
    ESP_LOGI(TAG, "HTTP server started on port %d", port);
    return true;
}

void AwtrixHttpServer::stop()
{
    if (m_handle)
    {
        httpd_stop(m_handle);
        m_handle = nullptr;
    }
}

/* ── route helpers ────────────────────────────────────────────── */
void AwtrixHttpServer::on(const std::string& uri, httpd_method_t method, Handler handler)
{
    s_routes.push_back({uri, method, std::move(handler)});
}

void AwtrixHttpServer::serveStatic(const std::string& uri, const std::string& filePath)
{
    on(uri, HTTP_GET, [filePath](httpd_req_t* req) -> esp_err_t
    {
        std::string fullPath = "/spiffs" + filePath;
        FILE* fp = fopen(fullPath.c_str(), "rb");
        if (!fp)
        {
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
        if (fseek(fp, 0, SEEK_END) != 0)
        {
            fclose(fp);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        long fsize = ftell(fp);
        if (fsize < 0)
        {
            fclose(fp);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        rewind(fp);
        char* buf = (char*)malloc(fsize);
        if (!buf)
        {
            fclose(fp);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        size_t n = fread(buf, 1, fsize, fp);
        fclose(fp);
        auto ends_with = [](const std::string& s, const char* suffix)
        {
            size_t sl = strlen(suffix);
            return s.size() >= sl && s.compare(s.size() - sl, sl, suffix) == 0;
        };
        const char* ct = "application/octet-stream";
        if (ends_with(filePath, ".html") || ends_with(filePath, ".htm")) ct = "text/html";
        else if (ends_with(filePath, ".js")) ct = "application/javascript";
        else if (ends_with(filePath, ".css")) ct = "text/css";
        else if (ends_with(filePath, ".json")) ct = "application/json";
        else if (ends_with(filePath, ".png")) ct = "image/png";
        else if (ends_with(filePath, ".jpg") || ends_with(filePath, ".jpeg")) ct = "image/jpeg";
        else if (ends_with(filePath, ".gif")) ct = "image/gif";
        else if (ends_with(filePath, ".ico")) ct = "image/x-icon";
        httpd_resp_set_type(req, ct);
        httpd_resp_send(req, buf, n);
        free(buf);
        return ESP_OK;
    });
}

void AwtrixHttpServer::onText(const std::string& uri, httpd_method_t method,
                              const std::string& content, const std::string& contentType)
{
    on(uri, method, [content, contentType](httpd_req_t* req) -> esp_err_t
    {
        httpd_resp_set_type(req, contentType.c_str());
        httpd_resp_send(req, content.c_str(), content.size());
        return ESP_OK;
    });
}

/* ── request helpers ──────────────────────────────────────────── */
std::string AwtrixHttpServer::getBody(httpd_req_t* req)
{
    int total_len = req->content_len;
    if (total_len <= 0) return "";
    char* buf = (char*)malloc(total_len + 1);
    if (!buf) return "";
    int received = httpd_req_recv(req, buf, total_len);
    std::string result;
    if (received > 0)
    {
        buf[received] = 0;
        result = buf;
    }
    free(buf);
    return result;
}

std::string AwtrixHttpServer::getHeader(httpd_req_t* req, const std::string& name)
{
    size_t len = httpd_req_get_hdr_value_len(req, name.c_str());
    if (len == 0) return "";
    char* buf = (char*)malloc(len + 1);
    if (!buf) return "";
    httpd_req_get_hdr_value_str(req, name.c_str(), buf, len + 1);
    std::string result(buf);
    free(buf);
    return result;
}

std::string AwtrixHttpServer::getQueryParam(httpd_req_t* req, const std::string& key)
{
    size_t len = httpd_req_get_url_query_len(req) + 1;
    if (len <= 1) return "";
    char* buf = (char*)malloc(len);
    if (!buf) return "";
    httpd_req_get_url_query_str(req, buf, len);

    std::string result;
    const size_t klen = key.size();
    /* Boundary-aware search: a key matches only when it sits at the start of
     * the query or immediately after '&', and is followed by '='. This avoids
     * prefix pollution (e.g. searching "ab" inside "abc=1&ab=2"). */
    char* p = buf;
    while (p && *p)
    {
        bool atBoundary = (p == buf) || (*(p - 1) == '&');
        if (atBoundary && strncmp(p, key.c_str(), klen) == 0 && p[klen] == '=')
        {
            char* valStart = p + klen + 1;
            char* end = strchr(valStart, '&');
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
esp_err_t AwtrixHttpServer::sendText(httpd_req_t* req, const char* text, int code)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_status(req, std::to_string(code).c_str());
    httpd_resp_send(req, text, strlen(text));
    return ESP_OK;
}

esp_err_t AwtrixHttpServer::sendJson(httpd_req_t* req, const char* json, int code)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, std::to_string(code).c_str());
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

esp_err_t AwtrixHttpServer::sendStatus(httpd_req_t* req, int code)
{
    httpd_resp_set_status(req, std::to_string(code).c_str());
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}