#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── Board identity ──────────────────────────────────────────── */
#define BOARD_ULANZI_ESP32S3   1
#define BOARD_TYPE             BOARD_ULANZI_ESP32S3

/* ── LED Matrix ──────────────────────────────────────────────── */
#define MATRIX_WIDTH  32
#define MATRIX_HEIGHT  8
#define MATRIX_PIN     5          /* GPIO5: WS2812 data */
#define MATRIX_TYPE    NEO_MATRIX_PROGRESSIVE  /* 4 panels of 8x8, progressive */

/* ── Buttons ─────────────────────────────────────────────────── */
#define BUTTON_LEFT_PIN    7
#define BUTTON_RIGHT_PIN   8
#define BUTTON_SELECT_PIN 10
#define BUTTON_RESET_PIN  13

/* ── Buzzer ──────────────────────────────────────────────────── */
#define BUZZER_PIN    15
#define BUZZER_CHAN    0

/* ── I2C (sensors) ──────────────────────────────────────────── */
#define I2C_SCL_PIN   10
#define I2C_SDA_PIN   11
#define I2C_FREQ_HZ   100000

/* ── ADC ─────────────────────────────────────────────────────── */
#define ADC_UNIT            ADC_UNIT_1
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_4  /* GPIO4 */
#define LDR_ADC_CHANNEL     ADC_CHANNEL_6  /* GPIO6 */

/* ── Defaults ────────────────────────────────────────────────── */
#define DEFAULT_HOSTNAME_LEN 32
#define DEFAULT_BRIGHTNESS   120
#define DEFAULT_MQTT_PORT    1883
#define DEFAULT_WEB_PORT     80
#define NTP_SERVER_DEFAULT   "pool.ntp.org"
#define NTP_TZ_DEFAULT       "CET-1CEST,M3.5.0,M10.5.0/3"
#define MAX_CUSTOM_APPS      16
#define MAX_NOTIFICATIONS     4

/* ── Global firmware version ─────────────────────────────────── */
#ifndef AWTRIX_VERSION
#define AWTRIX_VERSION "3.0.0-esp-idf"
#endif

/* ── Log tag macros ──────────────────────────────────────────── */
#define TAG_SYSTEM    "awtrix"
#define TAG_DISPLAY   "disp"
#define TAG_NET       "net"
#define TAG_HTTP      "httpd"
#define TAG_MQTT      "mqtt"
#define TAG_IO        "io"
#define TAG_STORE     "store"

#ifdef __cplusplus
}
#endif
