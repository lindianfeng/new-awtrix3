#include "awtrix_power.h"
#include "awtrix_hal.h"
#include "awtrix_command_bus.h"  /* E1: post ShowSleepScreen command directly */
#include "esp_sleep.h"
#include "esp_log.h"
#include <cJSON.h>

#define TAG TAG_SYSTEM

void awtrix_power_sleep(unsigned long seconds) {
    /* E1: queue the sleep-screen draw on the command bus so DisplayManager
     * processes it on its next tick. This used to fan out via EventBus.
     * showSleepScreen → main lambda → command bus — three indirections for
     * one queue post. Now the queue is the only path. */
    AwtrixCommand command;
    command.type = AwtrixCommandType::ShowSleepScreen;
    command.source = AWTRIX_COMMAND_SOURCE_SYSTEM;
    awtrix_command_bus_post(command, 0);
    vTaskDelay(pdMS_TO_TICKS(80));
    if (seconds > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    }
    /* Wake on SELECT button press (BUTTON_SELECT_PIN, active low) */
    esp_sleep_enable_ext1_wakeup((1ULL << BUTTON_SELECT_PIN), ESP_EXT1_WAKEUP_ANY_LOW);
    ESP_LOGI(TAG, "Entering deep sleep for %lu s", seconds);
    esp_deep_sleep_start();
}

void awtrix_power_sleep_parser(const char *json) {
    if (!json) { awtrix_power_sleep(60); return; }
    cJSON *doc = cJSON_Parse(json);
    if (!doc) { awtrix_power_sleep(60); return; }
    long s = 0;
    cJSON *v;
    if ((v = cJSON_GetObjectItem(doc, "seconds")) && cJSON_IsNumber(v)) s += v->valueint;
    if ((v = cJSON_GetObjectItem(doc, "minutes")) && cJSON_IsNumber(v)) s += v->valueint * 60;
    if ((v = cJSON_GetObjectItem(doc, "hours"))   && cJSON_IsNumber(v)) s += v->valueint * 3600;
    /* Pack P: accept the original Arduino key "sleep" (seconds, single field)
     * so existing tooling that posts {"sleep":60} keeps working. */
    if ((v = cJSON_GetObjectItem(doc, "sleep"))   && cJSON_IsNumber(v)) s += v->valueint;
    cJSON_Delete(doc);
    if (s <= 0) s = 60;
    awtrix_power_sleep((unsigned long)s);
}
