#include "awtrix_command_bus.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <new>

#define TAG "cmd_bus"

static constexpr UBaseType_t kAwtrixCommandQueueLen = 32;
static QueueHandle_t s_command_queue = nullptr;

void awtrix_command_bus_init(void) {
    if (s_command_queue) return;
    s_command_queue = xQueueCreate(kAwtrixCommandQueueLen, sizeof(AwtrixCommand *));
    if (!s_command_queue) {
        ESP_LOGE(TAG, "Failed to create command queue");
        return;
    }
    ESP_LOGI(TAG, "Command queue initialized (%u slots)", (unsigned)kAwtrixCommandQueueLen);
}

bool awtrix_command_bus_post(const AwtrixCommand &command, uint32_t timeout_ms) {
    if (!s_command_queue) awtrix_command_bus_init();
    if (!s_command_queue) return false;

    AwtrixCommand *copy = new (std::nothrow) AwtrixCommand(command);
    if (!copy) {
        ESP_LOGE(TAG, "Failed to allocate command type=%u", (unsigned)command.type);
        return false;
    }

    const TickType_t ticks = timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms);
    if (xQueueSend(s_command_queue, &copy, ticks) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full, dropping type=%u", (unsigned)command.type);
        delete copy;
        return false;
    }
    return true;
}

bool awtrix_command_bus_receive(AwtrixCommand &out, uint32_t timeout_ms) {
    if (!s_command_queue) return false;

    AwtrixCommand *command = nullptr;
    const TickType_t ticks = timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_command_queue, &command, ticks) != pdTRUE || !command) return false;

    out = *command;
    delete command;
    return true;
}

size_t awtrix_command_bus_depth(void) {
    if (!s_command_queue) return 0;
    return static_cast<size_t>(uxQueueMessagesWaiting(s_command_queue));
}
