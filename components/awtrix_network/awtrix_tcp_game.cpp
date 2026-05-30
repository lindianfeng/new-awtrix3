/**
 * Port of the original AWTRIX3 ServerManager.cpp TCP:8080 controller socket.
 *
 * The original opened a `WiFiServer TCPserver(8080)`, accepted one client at
 * a time, and forwarded newline-terminated lines into
 * `GameManager.ControllerInput(buf)` so a remote app could control the
 * AwtrixSays / SlotMachine mini-games over TCP. This port reproduces that
 * behaviour using lwIP BSD sockets running in a dedicated FreeRTOS task.
 *
 * Single-client semantics: when a new client connects while another is
 * already attached, the old client is dropped (mirroring the original).
 *
 * NOTE on character literals: line-ending sentinels are written as their
 * numeric values (0x0A / 0x0D) instead of '
' / '
' so that no editor or
 * file-conversion step can accidentally turn the escape sequence inside a
 * character literal into a bare line break.
 */

#include "awtrix_tcp_game.h"
#include "awtrix_games.h"
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <errno.h>

#define TAG "tcpgame"
#define BUFFER_SIZE 64

static int s_listen_sock = -1;
static int s_client_sock = -1;
static TaskHandle_t s_task_h = NULL;

static void close_client(void)
{
    if (s_client_sock >= 0)
    {
        shutdown(s_client_sock, 0);
        close(s_client_sock);
        s_client_sock = -1;
    }
}

static void tcp_game_task(void* arg)
{
    uint16_t port = (uint16_t)(uintptr_t)arg;

    struct sockaddr_in srv = {};
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_ANY);
    srv.sin_port = htons(port);

    s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_sock < 0)
    {
        ESP_LOGE(TAG, "socket(): errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(s_listen_sock, (struct sockaddr*)&srv, sizeof(srv)) != 0)
    {
        ESP_LOGE(TAG, "bind() port %u: errno %d", port, errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        vTaskDelete(NULL);
        return;
    }
    if (listen(s_listen_sock, 1) != 0)
    {
        ESP_LOGE(TAG, "listen(): errno %d", errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "TCP game controller listening on :%u", port);

    char buf[BUFFER_SIZE];
    int bufLen = 0;

    for (;;)
    {
        if (s_client_sock < 0)
        {
            struct sockaddr_in cli;
            socklen_t cl = sizeof(cli);
            int s = accept(s_listen_sock, (struct sockaddr*)&cli, &cl);
            if (s < 0)
            {
                ESP_LOGW(TAG, "accept(): errno %d", errno);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            s_client_sock = s;
            bufLen = 0;
            ESP_LOGI(TAG, "controller client connected");
        }

        unsigned char ch;
        int n = recv(s_client_sock, &ch, 1, 0);
        if (n <= 0)
        {
            ESP_LOGI(TAG, "controller client disconnected (n=%d, errno=%d)", n, errno);
            close_client();
            continue;
        }
        /* 0x0A == LF (line terminator), 0x0D == CR (skipped). */
        if (ch == 0x0A)
        {
            buf[bufLen] = 0;
            if (bufLen > 0) awtrix_game_controller_input(buf);
            bufLen = 0;
        }
        else if (ch != 0x0D)
        {
            if (bufLen < BUFFER_SIZE - 1)
            {
                buf[bufLen++] = (char)ch;
            }
            else
            {
                /* line too long — discard the partial buffer rather than
                 * silently truncating in the middle of a command. */
                bufLen = 0;
            }
        }
    }
}

void awtrix_tcp_game_start(uint16_t port)
{
    if (s_task_h) return;
    BaseType_t ok = xTaskCreate(tcp_game_task, "tcpgame", 4096,
                                (void*)(uintptr_t)port, 5, &s_task_h);
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "tcpgame task create failed");
        s_task_h = NULL;
    }
}

void awtrix_tcp_game_stop(void)
{
    close_client();
    if (s_listen_sock >= 0)
    {
        shutdown(s_listen_sock, 0);
        close(s_listen_sock);
        s_listen_sock = -1;
    }
    if (s_task_h)
    {
        vTaskDelete(s_task_h);
        s_task_h = NULL;
    }
}

void awtrix_tcp_game_send(const char* line)
{
    if (s_client_sock < 0 || !line) return;
    send(s_client_sock, line, strlen(line), 0);
}
