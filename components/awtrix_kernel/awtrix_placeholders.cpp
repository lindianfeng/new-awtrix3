#include "awtrix_placeholders.h"

#include "freertos/FreeRTOS.h"     /* portMUX_TYPE */
#include "esp_log.h"

#include <map>

#define TAG "placeholders"

namespace {

/* Hard cap to prevent runaway customApp payloads from exhausting heap.
 * 32 placeholders + 96-byte values = ~3 KB worst case. */
constexpr size_t kMaxPlaceholders   = 32;
constexpr size_t kMaxPlaceholderVal = 96;

struct Registry {
    portMUX_TYPE                       lock = portMUX_INITIALIZER_UNLOCKED;
    std::map<std::string, std::string> map;     /* topic → latest value */
};

/* Function-local-static singleton avoids a global ctor ordering hazard
 * (the AwtrixAppRegistry singleton already proved this pattern works). */
Registry &registry() {
    static Registry r;
    return r;
}

} // namespace

bool awtrix_placeholder_register(const std::string &topic) {
    if (topic.empty()) return false;
    Registry &r = registry();
    portENTER_CRITICAL(&r.lock);
    bool ok = true;
    if (r.map.find(topic) == r.map.end()) {
        if (r.map.size() >= kMaxPlaceholders) {
            ok = false;
        } else {
            r.map.emplace(topic, std::string());
        }
    }
    portEXIT_CRITICAL(&r.lock);
    if (!ok) {
        ESP_LOGW(TAG, "registry full (%u) — dropping topic %s",
                 (unsigned)kMaxPlaceholders, topic.c_str());
    }
    return ok;
}

void awtrix_placeholder_update(const std::string &topic, const std::string &value) {
    if (topic.empty()) return;
    Registry &r = registry();
    portENTER_CRITICAL(&r.lock);
    auto it = r.map.find(topic);
    if (it != r.map.end()) {
        it->second.assign(value, 0,
                          value.size() > kMaxPlaceholderVal ? kMaxPlaceholderVal
                                                            : value.size());
    }
    portEXIT_CRITICAL(&r.lock);
}

bool awtrix_placeholder_has(const std::string &topic) {
    if (topic.empty()) return false;
    Registry &r = registry();
    portENTER_CRITICAL(&r.lock);
    const bool has = r.map.find(topic) != r.map.end();
    portEXIT_CRITICAL(&r.lock);
    return has;
}

std::string awtrix_placeholder_get(const std::string &topic) {
    if (topic.empty()) return std::string();
    Registry &r = registry();
    std::string out;
    portENTER_CRITICAL(&r.lock);
    auto it = r.map.find(topic);
    if (it != r.map.end()) out = it->second;
    portEXIT_CRITICAL(&r.lock);
    return out;
}
