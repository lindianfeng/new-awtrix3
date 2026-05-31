#pragma once

#ifdef __cplusplus

#include <string>
#include <functional>
#include <map>
#include <vector>
#include "esp_http_server.h"

/* ── HTTP server wrapping esp_http_server ────────────────────────
 * Replaces Arduino WebServer + esp-fs-webserver.
 * Handlers receive (request *) and must respond directly.
 */

class AwtrixHttpServer
{
public:
    AwtrixHttpServer();
    ~AwtrixHttpServer();

    bool start(uint16_t port = 80);
    void stop();

    /* route registration */
    using Handler = std::function<esp_err_t(httpd_req_t *)>;

    void on(const std::string& uri, httpd_method_t method, Handler handler);

    /* convenience: register GET handler */
    void onGet(const std::string& uri, Handler h)
    {
        on(uri, HTTP_GET, std::move(h));
    }

    /* convenience: register POST handler */
    void onPost(const std::string& uri, Handler h)
    {
        on(uri, HTTP_POST, std::move(h));
    }

    /* serve a static file from SPIFFS */
    void serveStatic(const std::string& uri, const std::string& filePath);

    /* serve raw string content (for embedded HTML) */
    void onText(const std::string& uri, httpd_method_t method,
                const std::string& content, const std::string& contentType = "text/html");

    /* get the raw body from a POST/PUT request */
    static std::string getBody(httpd_req_t* req);
    static std::string getHeader(httpd_req_t* req, const std::string& name);
    static std::string getQueryParam(httpd_req_t* req, const std::string& key);

    /* send response helpers */
    static esp_err_t sendText(httpd_req_t* req, const char* text, int code = 200);
    static esp_err_t sendJson(httpd_req_t* req, const char* json, int code = 200);
    static esp_err_t sendStatus(httpd_req_t* req, int code);
    /* Pack M+: serve a gzip-compressed HTML/CSS/JS resource embedded as a
     * byte array in flash. Sets Content-Encoding: gzip so the browser
     * decompresses on the fly. Used by /setup (~12 KB → ~77 KB raw). */
    static esp_err_t sendGzipHtml(httpd_req_t* req, const void* buf, size_t len);

    httpd_handle_t handle() const { return m_handle; }

private:
    httpd_handle_t m_handle = nullptr;
    static esp_err_t static_handler(httpd_req_t* req);
};

/* ── Common response macros ───────────────────────────────────── */
#define HTTP_OK()        AwtrixHttpServer::sendStatus(req, 200)
#define HTTP_ERR()       AwtrixHttpServer::sendStatus(req, 500)

#endif /* __cplusplus */