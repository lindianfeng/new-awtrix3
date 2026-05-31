#include "awtrix_query_snapshot.h"

extern "C" const char *awtrix_core_query_apps_json(void) __attribute__((weak));
extern "C" const char *awtrix_core_query_settings_json(void) __attribute__((weak));
extern "C" const char *awtrix_core_query_stats_json(void) __attribute__((weak));
extern "C" const char *awtrix_core_query_apps_with_icon_json(void) __attribute__((weak));
extern "C" const char *awtrix_core_query_effect_names_json(void) __attribute__((weak));
extern "C" const char *awtrix_core_query_transition_names_json(void) __attribute__((weak));

static std::string call_or_empty(const char *(*fn)(void), const char *fallback) {
    if (!fn) return fallback ? fallback : "{}";
    const char *value = fn();
    return value ? value : (fallback ? fallback : "{}");
}

std::string awtrix_query_apps_json() {
    return call_or_empty(&awtrix_core_query_apps_json, "[]");
}

std::string awtrix_query_settings_json() {
    return call_or_empty(&awtrix_core_query_settings_json, "{}");
}

std::string awtrix_query_stats_json() {
    return call_or_empty(&awtrix_core_query_stats_json, "{}");
}

std::string awtrix_query_apps_with_icon_json() {
    return call_or_empty(&awtrix_core_query_apps_with_icon_json, "[]");
}

std::string awtrix_query_effect_names_json() {
    return call_or_empty(&awtrix_core_query_effect_names_json, "[]");
}

std::string awtrix_query_transition_names_json() {
    return call_or_empty(&awtrix_core_query_transition_names_json, "[]");
}
