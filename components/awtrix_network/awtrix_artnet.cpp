#include "awtrix_artnet.h"
#include "awtrix_globals.h"
#include "DisplayManager.h"
#include "matrix_cpp.h"
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

/* Minimal Art-Net packet parse: header "Art-Net\0" + opcode 0x5000 (OpDmx). */
static void artnet_task(void *) {
    struct sockaddr_in addr{};
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) { ESP_LOGE(TAG, "Art-Net socket failed"); vTaskDelete(nullptr); return; }
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(ARTNET_PORT);
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Art-Net bind failed"); close(s_sock); s_sock = -1;
        vTaskDelete(nullptr); return;
    }
    ESP_LOGI(TAG, "Art-Net listening on UDP/%d", ARTNET_PORT);
    uint8_t buf[600];
    while (s_run) {
        int n = recv(s_sock, buf, sizeof(buf), 0);
        if (n < 18) continue;
        if (memcmp(buf, "Art-Net\0", 8) != 0) continue;
        uint16_t op = (uint16_t)buf[8] | ((uint16_t)buf[9] << 8);
        if (op != 0x5000) continue; /* OpDmx */
        uint16_t universe = (uint16_t)buf[14] | ((uint16_t)buf[15] << 8);
        uint16_t len      = ((uint16_t)buf[16] << 8) | buf[17];
        if (len + 18 > n) len = n - 18;
        const uint8_t *dmx = buf + 18;

        if (!CONFIG.artnetMode) continue;
        auto *matrix = DisplayManager::get().getMatrix();
        if (!matrix) continue;
        CRGB *leds = matrix->getLeds();
        int total = matrix->width() * matrix->height();
        int startLed = (universe * 170);     /* 170 LEDs per DMX universe */
        for (int i = 0; i + 3 <= len && (startLed + i / 3) < total; i += 3) {
            CRGB &c = leds[startLed + i / 3];
            c.r = dmx[i + 0]; c.g = dmx[i + 1]; c.b = dmx[i + 2];
        }
        matrix->show();
    }
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    vTaskDelete(nullptr);
}

void awtrix_artnet_start(void) {
    if (s_run) return;
    s_run = true;
    xTaskCreate(artnet_task, "artnet", 4096, nullptr, 5, &s_task);
}
void awtrix_artnet_stop(void) {
    s_run = false;
    if (s_sock >= 0) shutdown(s_sock, SHUT_RDWR);
}
