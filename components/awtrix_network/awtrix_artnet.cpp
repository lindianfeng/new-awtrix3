/**
 * awtrix_artnet.cpp — Art-Net DMX-over-WiFi receiver.
 *
 * Lets a lighting console / DJ software push pixel frames straight to the
 * matrix over UDP without touching MQTT or HTTP. This is what the
 * `artnetMode` toggle gates: when on, DisplayManager::tick branches off
 * its usual app-rotation pipeline and copies the latest received frame
 * into the matrix instead.
 *
 * Protocol subset:
 *   - ART_DMX (op 0x5000) — single universe, port 6454 (0x1936).
 *   - ART_POLL / ART_SYNC / multiple universes are deliberately not
 *     implemented; the original AWTRIX firmware accepted the same
 *     minimum subset, and bigger consoles tolerate a missing POLL
 *     response by falling back to broadcast.
 *
 * Threading:
 *   - A dedicated FreeRTOS task (`art_net_task`) runs the recv() loop so
 *     UDP traffic doesn't compete with the 62 Hz display tick.
 *   - The most recent decoded frame lives in `s_latest_frame[]`, guarded
 *     by `s_frame_lock` (portMUX). `awtrix_artnet_take_frame()` copies
 *     under the lock and clears the "pending" flag in one atomic
 *     critical section.
 *
 * Exports:
 *   awtrix_artnet_start()          — boot-time hook called from main.cpp
 *   awtrix_artnet_take_frame(...)  — called from DisplayManager::tick when
 *                                    SYS_STATE == ArtNet (or
 *                                    CONFIG.artnetMode in legacy paths).
 */

#include "awtrix_artnet.h"
#include "awtrix_globals.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <cstring>

#define TAG TAG_NET
#define ARTNET_PORT 6454

static TaskHandle_t s_task = nullptr;
static int s_sock = -1;
static bool s_run = false;

static portMUX_TYPE s_frame_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_latest_frame[MATRIX_WIDTH * MATRIX_HEIGHT] = {0};
static int s_latest_pixels = MATRIX_WIDTH * MATRIX_HEIGHT;
static bool s_frame_pending = false;

/* Minimal Art-Net packet parse: header "Art-Net\0" + opcode 0x5000 (OpDmx). */
static void artnet_task(void*)
{
    struct sockaddr_in addr{};
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0)
    {
        ESP_LOGE(TAG, "Art-Net socket failed");
        vTaskDelete(nullptr);
        return;
    }
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(ARTNET_PORT);
    if (bind(s_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        ESP_LOGE(TAG, "Art-Net bind failed");
        close(s_sock);
        s_sock = -1;
        vTaskDelete(nullptr);
        return;
    }
    /* Bug 2: 1 s recv() timeout so s_run = false is honoured promptly
     * and stop() can reclaim the task without relying on shutdown()
     * to unblock the syscall. Without this, a quiet link kept the
     * task blocked indefinitely, leaking 4 KB of stack each cycle
     * the user toggled artnetMode. */
    struct timeval rcv_to = {.tv_sec = 1, .tv_usec = 0};
    if (setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to)) != 0)
    {
        ESP_LOGW(TAG, "Art-Net SO_RCVTIMEO failed (recv will block forever)");
    }
    ESP_LOGI(TAG, "Art-Net listening on UDP/%d", ARTNET_PORT);
    uint8_t buf[600];
    while (s_run)
    {
        int n = recv(s_sock, buf, sizeof(buf), 0);
        if (n < 0)
        {
            /* EAGAIN / EWOULDBLOCK = the 1 s timeout fired with no data;
             * loop back and re-check s_run. Any other negative return
             * means the socket has been shut down — exit the task. */
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            ESP_LOGW(TAG, "Art-Net recv error %d, stopping", errno);
            break;
        }
        if (n < 18) continue;
        if (memcmp(buf, "Art-Net\0", 8) != 0) continue;
        uint16_t op = (uint16_t)buf[8] | ((uint16_t)buf[9] << 8);
        if (op != 0x5000) continue; /* OpDmx */
        uint16_t universe = (uint16_t)buf[14] | ((uint16_t)buf[15] << 8);
        uint16_t len = ((uint16_t)buf[16] << 8) | buf[17];
        if (len + 18 > n) len = n - 18;
        const uint8_t* dmx = buf + 18;

        if (!CONFIG.artnetMode) continue;
        const int total = MATRIX_WIDTH * MATRIX_HEIGHT;
        const int startLed = universe * 170; /* 170 LEDs per DMX universe */
        if (startLed >= total) continue;

        portENTER_CRITICAL(&s_frame_lock);
        for (int i = 0; i + 3 <= len && (startLed + i / 3) < total; i += 3)
        {
            s_latest_frame[startLed + i / 3] = ((uint32_t)dmx[i + 0] << 16) |
                ((uint32_t)dmx[i + 1] << 8) |
                (uint32_t)dmx[i + 2];
        }
        s_latest_pixels = total;
        s_frame_pending = true;
        portEXIT_CRITICAL(&s_frame_lock);
    }
    if (s_sock >= 0)
    {
        close(s_sock);
        s_sock = -1;
    }
    vTaskDelete(nullptr);
}

void awtrix_artnet_start(void)
{
    if (s_run) return;
    s_run = true;
    xTaskCreate(artnet_task, "artnet", 4096, nullptr, 5, &s_task);
}

void awtrix_artnet_stop(void)
{
    s_run = false;
    if (s_sock >= 0) shutdown(s_sock, SHUT_RDWR);
}

bool awtrix_artnet_take_frame(uint32_t* rgb888, int max_pixels, int* out_pixels)
{
    if (!rgb888 || max_pixels <= 0) return false;

    bool hasFrame = false;
    int pixels = 0;
    portENTER_CRITICAL(&s_frame_lock);
    if (s_frame_pending)
    {
        pixels = s_latest_pixels;
        if (pixels > max_pixels) pixels = max_pixels;
        memcpy(rgb888, s_latest_frame, sizeof(uint32_t) * pixels);
        s_frame_pending = false;
        hasFrame = true;
    }
    portEXIT_CRITICAL(&s_frame_lock);

    if (out_pixels) *out_pixels = pixels;
    return hasFrame;
}
