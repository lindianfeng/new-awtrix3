#include "awtrix_dfplayer.h"
#include "awtrix_hal.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG TAG_IO

#ifndef DFPLAYER_UART_NUM
#define DFPLAYER_UART_NUM UART_NUM_2
#endif
#ifndef DFPLAYER_TX_PIN
#define DFPLAYER_TX_PIN 6
#endif
#ifndef DFPLAYER_RX_PIN
#define DFPLAYER_RX_PIN 7
#endif

static bool s_inited = false;
static bool s_playing = false;

static void dfp_send(uint8_t cmd, uint16_t param)
{
    uint8_t pkt[10] = {
        0x7E, 0xFF, 0x06, cmd, 0x00,
        (uint8_t)(param >> 8), (uint8_t)(param & 0xFF),
        0x00, 0x00, 0xEF
    };
    uart_write_bytes(DFPLAYER_UART_NUM, (const char*)pkt, sizeof(pkt));
}

bool awtrix_dfp_init(void)
{
    if (s_inited) return true;
    uart_config_t cfg = {};
    cfg.baud_rate = 9600;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    if (uart_driver_install(DFPLAYER_UART_NUM, 1024, 0, 0, NULL, 0) != ESP_OK) return false;
    if (uart_param_config(DFPLAYER_UART_NUM, &cfg) != ESP_OK) return false;
    if (uart_set_pin(DFPLAYER_UART_NUM, DFPLAYER_TX_PIN, DFPLAYER_RX_PIN,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)
        return false;
    vTaskDelay(pdMS_TO_TICKS(1000)); /* DFPlayer needs ~1s to boot */
    dfp_send(0x09, 0x0002); /* select SD as source */
    vTaskDelay(pdMS_TO_TICKS(200));
    s_inited = true;
    ESP_LOGI(TAG, "DFPlayer ready (UART%d, TX=%d RX=%d)",
             (int)DFPLAYER_UART_NUM, DFPLAYER_TX_PIN, DFPLAYER_RX_PIN);
    return true;
}

void awtrix_dfp_set_volume(uint8_t v)
{
    if (!s_inited) return;
    if (v > 30) v = 30;
    dfp_send(0x06, v);
}

void awtrix_dfp_play_file_number(uint16_t n)
{
    if (!s_inited) return;
    dfp_send(0x03, n);
    s_playing = true;
}

void awtrix_dfp_play_folder(uint8_t folder, uint8_t file)
{
    if (!s_inited) return;
    dfp_send(0x0F, (uint16_t)((folder << 8) | file));
    s_playing = true;
}

void awtrix_dfp_stop(void)
{
    if (!s_inited) return;
    dfp_send(0x16, 0);
    s_playing = false;
}

bool awtrix_dfp_is_playing(void) { return s_playing; }
