#include "awtrix_power.h"
#include "awtrix_hal.h"
#include "awtrix_events.h"   /* EVENTS.showSleepScreen() — replaces direct DisplayManager.h dep */
#include "esp_sleep.h"
#include "esp_log.h"
#include <cJSON.h>

#define TAG TAG_SYSTEM

void awtrix_power_sleep(unsigned long seconds)
{
    EVENTS.showSleepScreen();
    if (seconds > 0)
    {
        esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    }
    /* Wake on SELECT button press (BUTTON_SELECT_PIN, active low) */
    esp_sleep_enable_ext1_wakeup((1ULL << BUTTON_SELECT_PIN), ESP_EXT1_WAKEUP_ANY_LOW);
    ESP_LOGI(TAG, "Entering deep sleep for %lu s", seconds);
    esp_deep_sleep_start();
}

void awtrix_power_sleep_parser(const char* json)
{
    if (!json)
    {
        awtrix_power_sleep(60);
        return;
    }
    cJSON* doc = cJSON_Parse(json);
    if (!doc)
    {
        awtrix_power_sleep(60);
        return;
    }
    long s = 0;
    cJSON* v;
    if ((v = cJSON_GetObjectItem(doc, "seconds")) && cJSON_IsNumber(v)) s += v->valueint;
    if ((v = cJSON_GetObjectItem(doc, "minutes")) && cJSON_IsNumber(v)) s += v->valueint * 60;
    if ((v = cJSON_GetObjectItem(doc, "hours")) && cJSON_IsNumber(v)) s += v->valueint * 3600;
    cJSON_Delete(doc);
    if (s <= 0) s = 60;
    awtrix_power_sleep((unsigned long)s);
}
