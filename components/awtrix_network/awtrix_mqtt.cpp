/**
 * Port of original AWTRIX3 src/MQTTManager.cpp on top of ESP-IDF's esp-mqtt
 * client. The original used PubSubClient + Arduino strings; this port keeps
 * the same topic layout and command surface (~26 topics).
 *
 * Subscribed topics (all relative to MQTT_PREFIX):
 *   /power /sleep /notify /notify/dismiss /apps /switch
 *   /sendscreen /settings /r2d2 /nextapp /previousapp /rtttl /sound
 *   /indicator1 /indicator2 /indicator3 /moodlight /reboot /custom/+
 *
 * Published telemetry every CONFIG.statsInterval ms:
 *   stats/version stats/uptime stats/temperature stats/humidity stats/battery
 *   stats/lux stats/effects stats/transitions
 */
#include "awtrix_mqtt.h"
#include "awtrix_globals.h"
#include "awtrix_utils.h"
#include "awtrix_network.h"      /* awtrix_wifi_get_ip_str() */
#include "awtrix_command_bus.h"
#include "awtrix_display_snapshot.h"
#include "awtrix_query_snapshot.h"
#include "awtrix_protocol.h"
#include "awtrix_periphery.h"
#include "awtrix_placeholders.h"  /* P1-D: customApp {{topic}} live MQTT value cache */
#include "mqtt_client.h"
#include <cJSON.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"             /* esp_wifi_sta_get_ap_info() */
#include "esp_system.h"           /* esp_get_free_heap_size() */
#include <string>
#include <vector>
#include <cstring>
#include <cstdarg>
#include <cstdio>

#define TAG TAG_MQTT

static esp_mqtt_client_handle_t s_client = nullptr;
static bool s_connected = false;
static std::string s_prefix;
static std::string s_mqtt_uri;
static std::string s_client_id;
static std::string s_username;
static std::string s_password;
static std::string s_will_topic;
static uint64_t s_lastStats = 0;
static std::vector<std::string> s_pending_subs;

static constexpr size_t kMaxMqttJsonPayload = 16 * 1024;
static constexpr size_t kMaxMqttSmallPayload = 2048;
static constexpr size_t kMaxMqttCustomName = 48;
static constexpr size_t kMaxPendingSubscriptions = 16;
static constexpr size_t kMaxTopicSuffixLen = 96;
static constexpr const char* kAvailabilitySuffix = "status";
static constexpr const char* kAvailabilityOnline = "online";
static constexpr const char* kAvailabilityOffline = "offline";

static bool is_safe_topic_suffix(const char* suffix)
{
    if (!suffix || !*suffix) return false;
    const size_t len = strlen(suffix);
    if (len == 0 || len > kMaxTopicSuffixLen) return false;
    if (strstr(suffix, "..")) return false;
    for (const char* p = suffix; *p; ++p)
    {
        const char ch = *p;
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
            ch == '/' || ch == '.';
        if (!ok || ch == '+' || ch == '#') return false;
    }
    return true;
}

static bool is_valid_custom_name(const std::string& name)
{
    if (name.empty() || name.size() > kMaxMqttCustomName) return false;
    if (name.find("..") != std::string::npos) return false;
    for (char ch : name)
    {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
        if (!ok) return false;
    }
    return true;
}

static bool mqtt_payload_within_limit(const std::string& topic, const std::string& payload, size_t limit)
{
    if (payload.size() <= limit) return true;
    ESP_LOGW(TAG, "Dropped MQTT payload for %s: %u > %u bytes",
             topic.c_str(), (unsigned)payload.size(), (unsigned)limit);
    return false;
}

static bool mqtt_payload_is_json_object(const std::string& topic, const std::string& payload)
{
    if (payload.empty()) return true;
    cJSON* doc = cJSON_Parse(payload.c_str());
    if (!doc)
    {
        ESP_LOGW(TAG, "Dropped MQTT payload for %s: invalid JSON", topic.c_str());
        return false;
    }
    const bool ok = cJSON_IsObject(doc);
    cJSON_Delete(doc);
    if (!ok) ESP_LOGW(TAG, "Dropped MQTT payload for %s: JSON object expected", topic.c_str());
    return ok;
}

static bool mqtt_accept_json_payload(const std::string& topic, const std::string& payload,
                                     size_t limit = kMaxMqttJsonPayload)
{
    return mqtt_payload_within_limit(topic, payload, limit) && mqtt_payload_is_json_object(topic, payload);
}

static bool mqtt_accept_protocol_payload(const std::string& fullTopic, const char* suffix, const std::string& payload,
                                         size_t limit = kMaxMqttJsonPayload)
{
    if (!mqtt_payload_within_limit(fullTopic, payload, limit)) return false;
    AwtrixProtocolError err;
    if (awtrix_protocol_validate_mqtt_body(suffix, payload.c_str(), err)) return true;
    ESP_LOGW(TAG, "Dropped MQTT payload for %s: %s", fullTopic.c_str(), err.message.c_str());
    return false;
}

/* Hardening 13: dangerous commands (reboot / power-off / sleep) require
 * the payload to include a JSON field "uid" or "id" that matches
 * CONFIG.uniqueID. This is *not* a full authentication mechanism — anyone
 * who can subscribe to `stats/uniqueid` will see it — but it raises the
 * cost of a broadcast attack from "publish one message to destroy every
 * device on the broker" to "first observe each device's unique ID before
 * targeting it". MQTT broker ACLs remain the right primary defense.
 *
 * Empty uniqueID (boot grace period before loadDevSettings has run)
 * rejects everything so the device can't be wiped during its weakest
 * window. */
static bool mqtt_payload_authorized(const std::string& payload, const char* command)
{
    /* Snapshot uniqueID under the config lock — same pattern as the
     * MQTT discovery code. uniqueID is set once at boot, so the
     * snapshot stays valid for the rest of this call. */
    std::string expected;
    {
        AwtrixConfig::Guard guard(CONFIG);
        expected = CONFIG.uniqueID;
    }
    if (expected.empty())
    {
        ESP_LOGW(TAG, "Reject MQTT %s: uniqueID not yet set", command);
        return false;
    }
    cJSON* doc = cJSON_Parse(payload.c_str());
    if (!doc)
    {
        ESP_LOGW(TAG, "Reject MQTT %s: payload not JSON", command);
        return false;
    }
    cJSON* uid = cJSON_GetObjectItem(doc, "uid");
    if (!uid || !cJSON_IsString(uid)) uid = cJSON_GetObjectItem(doc, "id");
    bool ok = uid && cJSON_IsString(uid) && uid->valuestring &&
        expected == uid->valuestring;
    cJSON_Delete(doc);
    if (!ok)
    {
        ESP_LOGW(TAG, "Reject MQTT %s: uid mismatch (need device uniqueID)", command);
    }
    return ok;
}

static std::string make_topic(const char* suffix)
{
    std::string t = s_prefix;
    if (suffix && suffix[0] != '/') t += '/';
    t += suffix ? suffix : "";
    return t;
}

static std::string availability_topic()
{
    return make_topic(kAvailabilitySuffix);
}

static void publish_availability(const char* state)
{
    if (!s_client) return;
    const std::string topic = availability_topic();
    esp_mqtt_client_publish(s_client, topic.c_str(), state ? state : kAvailabilityOffline, 0, 1, 1);
}

static std::string json_escape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char ch : value)
    {
        switch (ch)
        {
        case '\\': out += "\\\\";
            break;
        case '"': out += "\\\"";
            break;
        case '\b': out += "\\b";
            break;
        case '\f': out += "\\f";
            break;
        case '\n': out += "\\n";
            break;
        case '\r': out += "\\r";
            break;
        case '\t': out += "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                char buf[7];
                snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            }
            else
            {
                out += (char)ch;
            }
            break;
        }
    }
    return out;
}

static std::string safe_object_id(const std::string& value, const char* fallback)
{
    std::string out;
    out.reserve(value.size());
    for (char ch : value)
    {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        out += ok ? ch : '_';
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? std::string(fallback ? fallback : "awtrix") : out;
}

static std::string string_format(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    const int needed = vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (needed <= 0)
    {
        va_end(args);
        return std::string();
    }
    std::string out((size_t)needed + 1, '\0');
    vsnprintf(out.data(), out.size(), fmt, args);
    va_end(args);
    out.resize((size_t)needed);
    return out;
}

/* ── inbound message dispatcher (replays MQTTManager processMqttMessage) */
static void process_message(const std::string& topic, const std::string& payload)
{
    auto& per = PeripheryManager::get();

    auto starts_with = [&](const std::string& t, const std::string& p)
    {
        return t.size() >= p.size() && t.compare(0, p.size(), p) == 0;
    };
    auto equals = [&](const std::string& t, const char* sfx)
    {
        std::string full = make_topic(sfx);
        return t == full;
    };

    auto
    post = [&](AwtrixCommand command)
    {
        command.source = AWTRIX_COMMAND_SOURCE_MQTT;
        if (!awtrix_command_bus_post(command, 0))
        {
            ESP_LOGW(TAG, "Dropped MQTT command for topic %s", topic.c_str());
        }
    };
    auto parseCommand = [&](const char* suffix, const std::string& payloadText, AwtrixCommand& command)
    {
        AwtrixProtocolError err;
        if (awtrix_protocol_parse_mqtt(suffix, payloadText.c_str(), command, err)) return true;
        ESP_LOGW(TAG, "Dropped MQTT command for %s: %s", suffix ? suffix : "<null>", err.message.c_str());
        return false;
    };
    auto postParsed = [&](const char* suffix, const std::string& payloadText)
    {
        AwtrixCommand command;
        if (parseCommand(suffix, payloadText, command)) post(command);
    };
    auto executeLocalParsed = [&](const char* suffix, const std::string& payloadText)
    {
        AwtrixCommand command;
        if (!parseCommand(suffix, payloadText, command)) return;
        switch (command.type)
        {
        case AwtrixCommandType::R2D2: per.r2d2(command.payload.c_str());
            break;
        case AwtrixCommandType::Rtttl: per.playRTTTL(command.payload.c_str());
            break;
        case AwtrixCommandType::Sound: per.parseSound(command.payload.c_str());
            break;
        case AwtrixCommandType::Reboot: esp_restart();
            break;
        default: post(command);
            break;
        }
    };

    {
        AwtrixConfig::Guard guard(CONFIG);
        CONFIG.receivedMessages++;
    }
    /* P1-D: customApp {{topic}} placeholder cache. If this topic was
     * registered by parseCustomPage, snapshot the payload so the next
     * render frame picks it up. Falling through to the dispatch chain is
     * intentional — a placeholder topic and an AWTRIX command topic could
     * in theory both match, and we want both code paths to run. */
    if (awtrix_placeholder_has(topic))
    {
        awtrix_placeholder_update(topic, payload);
    }
    if (equals(topic, "notify"))
    {
        if (!mqtt_accept_protocol_payload(topic, "notify", payload)) return;
        postParsed("notify", payload);
    }
    else if (equals(topic, "notify/dismiss"))
    {
        postParsed("notify/dismiss", payload);
    }
    else if (equals(topic, "apps"))
    {
        if (!mqtt_accept_protocol_payload(topic, "apps", payload)) return;
        postParsed("apps", payload);
    }
    else if (equals(topic, "switch"))
    {
        if (!mqtt_accept_protocol_payload(topic, "switch", payload)) return;
        postParsed("switch", payload);
    }
    else if (equals(topic, "sendscreen")) awtrix_mqtt_publish("screen", DISPLAY_SNAPSHOT.screenJson().c_str());
    else if (equals(topic, "settings"))
    {
        if (!mqtt_accept_protocol_payload(topic, "settings", payload)) return;
        postParsed("settings", payload);
    }
    else if (equals(topic, "r2d2"))
    {
        if (!mqtt_payload_within_limit(topic, payload, kMaxMqttSmallPayload)) return;
        executeLocalParsed("r2d2", payload);
    }
    else if (equals(topic, "nextapp"))
    {
        postParsed("nextapp", payload);
    }
    else if (equals(topic, "previousapp"))
    {
        postParsed("previousapp", payload);
    }
    else if (equals(topic, "rtttl"))
    {
        if (!mqtt_payload_within_limit(topic, payload, kMaxMqttSmallPayload)) return;
        executeLocalParsed("rtttl", payload);
    }
    else if (equals(topic, "power"))
    {
        if (!mqtt_accept_protocol_payload(topic, "power", payload)) return;
        if (!mqtt_payload_authorized(payload, "power")) return;
        postParsed("power", payload);
    }
    else if (equals(topic, "sleep"))
    {
        if (!mqtt_accept_protocol_payload(topic, "sleep", payload)) return;
        if (!mqtt_payload_authorized(payload, "sleep")) return;
        postParsed("sleep", payload);
    }
    else if (equals(topic, "indicator1"))
    {
        if (!mqtt_accept_protocol_payload(topic, "indicator1", payload)) return;
        postParsed("indicator1", payload);
    }
    else if (equals(topic, "indicator2"))
    {
        if (!mqtt_accept_protocol_payload(topic, "indicator2", payload)) return;
        postParsed("indicator2", payload);
    }
    else if (equals(topic, "indicator3"))
    {
        if (!mqtt_accept_protocol_payload(topic, "indicator3", payload)) return;
        postParsed("indicator3", payload);
    }
    else if (equals(topic, "moodlight"))
    {
        if (!mqtt_accept_protocol_payload(topic, "moodlight", payload)) return;
        postParsed("moodlight", payload);
    }
    else if (equals(topic, "reboot"))
    {
        if (!mqtt_payload_authorized(payload, "reboot")) return;
        executeLocalParsed("reboot", payload);
    }
    else if (equals(topic, "sound"))
    {
        if (!mqtt_accept_json_payload(topic, payload)) return;
        executeLocalParsed("sound", payload);
    }
    /* Pack O: shortcut-write topics ported from the original MQTTManager.
     * Payload is the raw value (no JSON wrapper). Internally we synthesize
     * a single-key settings JSON so the rest of the pipeline (validation,
     * persistence) stays unchanged. */
    else if (equals(topic, "brightness"))
    {
        if (!mqtt_payload_within_limit(topic, payload, kMaxMqttSmallPayload)) return;
        std::string body = "{\"BRI\":" + payload + "}";
        AwtrixProtocolError err;
        if (!awtrix_protocol_validate_mqtt_body("settings", body.c_str(), err))
        {
            ESP_LOGW(TAG, "Bad brightness payload: %s", err.message.c_str());
            return;
        }
        postParsed("settings", body);
    }
    else if (equals(topic, "timeformat"))
    {
        if (!mqtt_payload_within_limit(topic, payload, kMaxMqttSmallPayload)) return;
        /* Quote string payloads automatically (TFORMAT is a strftime spec). */
        std::string body = "{\"TFORMAT\":\"" + json_escape(payload) + "\"}";
        postParsed("settings", body);
    }
    else if (equals(topic, "dateformat"))
    {
        if (!mqtt_payload_within_limit(topic, payload, kMaxMqttSmallPayload)) return;
        std::string body = "{\"DFORMAT\":\"" + json_escape(payload) + "\"}";
        postParsed("settings", body);
    }
    else if (starts_with(topic, make_topic("custom")))
    {
        /* "<prefix>/custom/<name>" → name = trailing segment */
        const std::string prefix = make_topic("custom/");
        if (topic.size() > prefix.size())
        {
            const std::string name = topic.c_str() + prefix.size();
            if (!is_valid_custom_name(name))
            {
                ESP_LOGW(TAG, "Dropped MQTT custom app with invalid name: %s", name.c_str());
                return;
            }
            if (!mqtt_accept_json_payload(topic, payload)) return;
            postParsed(("custom/" + name).c_str(), payload);
        }
    }
}

static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                               int32_t event_id, void* event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "MQTT connected");
        publish_availability(kAvailabilityOnline);
        /* core subscriptions */
        {
            const char* core[] = {
                "notify", "notify/dismiss", "apps", "switch", "sendscreen",
                "settings", "r2d2", "nextapp", "previousapp", "rtttl", "power", "sleep",
                "indicator1", "indicator2", "indicator3", "moodlight", "reboot", "sound",
                /* Pack O: shortcut-write topics ported from the original
                 * MQTTManager. payload is the raw value (no JSON wrapper). */
                "brightness", "timeformat", "dateformat",
                "custom/+",
            };
            for (auto t : core)
            {
                std::string full = make_topic(t);
                esp_mqtt_client_subscribe(s_client, full.c_str(), 0);
            }
            for (auto& t : s_pending_subs) esp_mqtt_client_subscribe(s_client, t.c_str(), 0);
        }
        /* HA discovery (optional) */
        {
            bool haDiscovery = false;
            std::string version;
            {
                AwtrixConfig::Guard guard(CONFIG);
                haDiscovery = CONFIG.ha_discovery;
                version = CONFIG.version ? CONFIG.version : "";
            }
            if (haDiscovery) awtrix_mqtt_publish_ha_discovery();
            awtrix_mqtt_publish("stats/version", version.c_str());
        }
        /* presence */
        awtrix_mqtt_publish("stats/effects", awtrix_query_effect_names_json().c_str());
        awtrix_mqtt_publish("stats/transitions", awtrix_query_transition_names_json().c_str());
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_DATA:
        {
            std::string topic(ev->topic, ev->topic_len);
            std::string payload(ev->data, ev->data_len);
            ESP_LOGD(TAG, "MQTT msg: %s = %.*s", topic.c_str(), ev->data_len, ev->data);
            process_message(topic, payload);
            break;
        }
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;
    default: break;
    }
}

void awtrix_mqtt_init(void)
{
    std::string mqttHost;
    uint16_t mqttPort = 1883;
    {
        AwtrixConfig::Guard guard(CONFIG);
        mqttHost = CONFIG.mqtt_host;
        mqttPort = CONFIG.mqtt_port;
        s_prefix = CONFIG.mqtt_prefix.empty() ? std::string("awtrix") : CONFIG.mqtt_prefix;
        s_client_id = CONFIG.hostname;
        s_username = CONFIG.mqtt_user;
        s_password = CONFIG.mqtt_pass;
    }

    if (mqttHost.empty())
    {
        ESP_LOGI(TAG, "MQTT disabled (no host configured)");
        return;
    }

    s_mqtt_uri = string_format("mqtt://%s:%u", mqttHost.c_str(), mqttPort);
    s_will_topic = availability_topic();

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = s_mqtt_uri.c_str();
    if (!s_username.empty()) cfg.credentials.username = s_username.c_str();
    if (!s_password.empty()) cfg.credentials.authentication.password = s_password.c_str();
    cfg.credentials.client_id = s_client_id.c_str();
    cfg.session.keepalive = 30;
    cfg.session.last_will.topic = s_will_topic.c_str();
    cfg.session.last_will.msg = kAvailabilityOffline;
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = true;
    cfg.network.disable_auto_reconnect = false;
    cfg.network.reconnect_timeout_ms = 5000;

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client)
    {
        ESP_LOGE(TAG, "MQTT client init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_event_handler, nullptr);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "MQTT started → %s", s_mqtt_uri.c_str());
}

void awtrix_mqtt_tick(void)
{
    if (!s_client || !s_connected) return;
    uint64_t now = esp_timer_get_time() / 1000;

    uint32_t statsInterval = 0;
    float currentTemp = 0.0f;
    float currentHum = 0.0f;
    float currentLux = 0.0f;
    uint8_t batteryPercent = 0;
    int brightness = 0;
    {
        AwtrixConfig::Guard guard(CONFIG);
        statsInterval = CONFIG.statsInterval;
        currentTemp = CONFIG.currentTemp;
        currentHum = CONFIG.currentHum;
        currentLux = CONFIG.currentLux;
        batteryPercent = CONFIG.batteryPercent;
        brightness = CONFIG.brightness;
    }

    if (now - s_lastStats >= (uint64_t)statsInterval)
    {
        s_lastStats = now;
        char buf[64];
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)(now / 1000));
        awtrix_mqtt_publish("stats/uptime", buf);
        snprintf(buf, sizeof(buf), "%.2f", currentTemp);
        awtrix_mqtt_publish("stats/temperature", buf);
        snprintf(buf, sizeof(buf), "%.2f", currentHum);
        awtrix_mqtt_publish("stats/humidity", buf);
        snprintf(buf, sizeof(buf), "%u", (unsigned)batteryPercent);
        awtrix_mqtt_publish("stats/battery", buf);
        snprintf(buf, sizeof(buf), "%.0f", currentLux);
        awtrix_mqtt_publish("stats/lux", buf);

        /* Free heap. */
        snprintf(buf, sizeof(buf), "%u", (unsigned)esp_get_free_heap_size());
        awtrix_mqtt_publish("stats/ram", buf);

        /* Brightness. */
        snprintf(buf, sizeof(buf), "%d", brightness);
        awtrix_mqtt_publish("stats/brightness", buf);

        /* WiFi RSSI. */
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        {
            snprintf(buf, sizeof(buf), "%d", (int)ap.rssi);
            awtrix_mqtt_publish("stats/rssi", buf);
        }

        /* IP address. */
        char ip[40] = {0};
        awtrix_wifi_get_ip_str(ip, sizeof(ip));
        if (ip[0]) awtrix_mqtt_publish("stats/ip", ip);

        /* P1-F: device unique ID — published every cycle so HA's myOwnID
         * sensor doesn't go "Unknown" when the entity is reloaded.
         * uniqueID is set once at boot and never changes, so a snapshot
         * outside the config guard is safe. */
        const std::string& uid = CONFIG.uniqueID;
        if (!uid.empty()) awtrix_mqtt_publish("stats/uniqueid", uid.c_str());
    }
}

bool awtrix_mqtt_is_connected(void) { return s_connected; }

void awtrix_mqtt_publish(const char* suffix, const char* payload)
{
    if (!s_client) return;
    std::string t = make_topic(suffix);
    esp_mqtt_client_publish(s_client, t.c_str(), payload ? payload : "", 0, 0, 0);
}

void awtrix_mqtt_subscribe(const char* suffix)
{
    if (!is_safe_topic_suffix(suffix))
    {
        ESP_LOGW(TAG, "Rejected unsafe MQTT subscription suffix");
        return;
    }
    std::string t = make_topic(suffix);
    if (s_client && s_connected)
    {
        esp_mqtt_client_subscribe(s_client, t.c_str(), 0);
        return;
    }
    for (const auto& pending : s_pending_subs)
    {
        if (pending == t) return;
    }
    if (s_pending_subs.size() >= kMaxPendingSubscriptions)
    {
        ESP_LOGW(TAG, "Rejected MQTT subscription %s: pending list full", t.c_str());
        return;
    }
    s_pending_subs.push_back(t);
}

void awtrix_mqtt_publish_ha_discovery(void)
{
    if (!s_client) return;

    std::string haPrefix;
    std::string host;
    std::string ver;
    {
        AwtrixConfig::Guard guard(CONFIG);
        haPrefix = CONFIG.ha_prefix.empty() ? std::string("homeassistant") : CONFIG.ha_prefix;
        host = CONFIG.hostname;
        ver = CONFIG.version ? CONFIG.version : "";
    }

    const std::string objectId = safe_object_id(host, "awtrix");
    const std::string hostJson = json_escape(host);
    const std::string objectIdJson = json_escape(objectId);
    const std::string prefixJson = json_escape(s_prefix);
    const std::string availabilityTopicJson = json_escape(availability_topic());
    const std::string dev = string_format(
        "\"availability_topic\":\"%s\",\"payload_available\":\"%s\",\"payload_not_available\":\"%s\","
        "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"AWTRIX %s\","
        "\"manufacturer\":\"AWTRIX3 ESP-IDF\",\"model\":\"Ulanzi S3\",\"sw_version\":\"%s\"}",
        availabilityTopicJson.c_str(), kAvailabilityOnline, kAvailabilityOffline,
        hostJson.c_str(), hostJson.c_str(), json_escape(ver).c_str());

    auto pub = [&](const char* component, const char* id, const std::string& payload)
    {
        std::string topic = haPrefix + "/" + component + "/" + objectId + "/" + id + "/config";
        esp_mqtt_client_publish(s_client, topic.c_str(), payload.c_str(), 0, 0, 1);
    };

    auto sensor = [&](const char* id, const char* name, const char* stateSfx,
                      const char* unit, const char* deviceClass)
    {
        const std::string deviceClassFragment = deviceClass
                                                    ? string_format(",\"device_class\":\"%s\"", deviceClass)
                                                    : std::string();
        pub("sensor", id, string_format(
                "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"state_topic\":\"%s/%s\","
                "\"unit_of_measurement\":\"%s\"%s,%s}",
                name, objectIdJson.c_str(), id, prefixJson.c_str(), stateSfx, unit ? unit : "",
                deviceClassFragment.c_str(), dev.c_str()));
    };

    auto binary = [&](const char* id, const char* name, const char* stateSfx)
    {
        pub("binary_sensor", id, string_format(
                "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"state_topic\":\"%s/%s\","
                "\"payload_on\":\"PRESSED\",\"payload_off\":\"RELEASED\",%s}",
                name, objectIdJson.c_str(), id, prefixJson.c_str(), stateSfx, dev.c_str()));
    };

    auto indicatorLight = [&](int n)
    {
        const std::string id = string_format("indicator%d", n);
        pub("light", id.c_str(), string_format(
                "{\"name\":\"AWTRIX Indicator %d\",\"unique_id\":\"%s_%s\","
                "\"schema\":\"json\",\"command_topic\":\"%s/%s\","
                "\"state_topic\":\"%s/stats/%s\",\"rgb\":true,\"brightness\":false,%s}",
                n, objectIdJson.c_str(), id.c_str(), prefixJson.c_str(), id.c_str(),
                prefixJson.c_str(), id.c_str(), dev.c_str()));
    };

    pub("light", "matrix", string_format(
            "{\"name\":\"AWTRIX\",\"unique_id\":\"%s_light\",\"schema\":\"json\","
            "\"command_topic\":\"%s/power\",\"state_topic\":\"%s/stats/power\","
            "\"brightness\":true,\"rgb\":true,%s}",
            objectIdJson.c_str(), prefixJson.c_str(), prefixJson.c_str(), dev.c_str()));

    sensor("temperature", "AWTRIX Temperature", "stats/temperature", "°C", "temperature");
    sensor("humidity", "AWTRIX Humidity", "stats/humidity", "%", "humidity");
    sensor("battery", "AWTRIX Battery", "stats/battery", "%", "battery");
    sensor("lux", "AWTRIX Lux", "stats/lux", "lx", "illuminance");
    sensor("uptime", "AWTRIX Uptime", "stats/uptime", "s", "duration");
    sensor("rssi", "AWTRIX RSSI", "stats/rssi", "dBm", "signal_strength");

    pub("sensor", "ip", string_format(
            "{\"name\":\"AWTRIX IP\",\"unique_id\":\"%s_ip\",\"state_topic\":\"%s/stats/ip\",%s}",
            objectIdJson.c_str(), prefixJson.c_str(), dev.c_str()));
    pub("sensor", "version", string_format(
            "{\"name\":\"AWTRIX Version\",\"unique_id\":\"%s_ver\",\"state_topic\":\"%s/stats/version\",%s}",
            objectIdJson.c_str(), prefixJson.c_str(), dev.c_str()));
    pub("sensor", "ram", string_format(
            "{\"name\":\"AWTRIX Free RAM\",\"unique_id\":\"%s_ram\",\"state_topic\":\"%s/stats/ram\","
            "\"unit_of_measurement\":\"B\",%s}",
            objectIdJson.c_str(), prefixJson.c_str(), dev.c_str()));
    pub("sensor", "currentApp", string_format(
            "{\"name\":\"AWTRIX Current App\",\"unique_id\":\"%s_app\",\"state_topic\":\"%s/stats/currentApp\",%s}",
            objectIdJson.c_str(), prefixJson.c_str(), dev.c_str()));

    indicatorLight(1);
    indicatorLight(2);
    indicatorLight(3);

    binary("button_left", "AWTRIX Button Left", "stats/btn_left");
    binary("button_select", "AWTRIX Button Select", "stats/btn_select");
    binary("button_right", "AWTRIX Button Right", "stats/btn_right");

    pub("number", "brightness", string_format(
            "{\"name\":\"AWTRIX Brightness\",\"unique_id\":\"%s_brightness\","
            "\"command_topic\":\"%s/settings\",\"state_topic\":\"%s/stats/brightness\","
            "\"command_template\":\"{\\\"BRI\\\":{{value}}}\","
            "\"min\":0,\"max\":255,\"step\":1,%s}",
            objectIdJson.c_str(), prefixJson.c_str(), prefixJson.c_str(), dev.c_str()));

    /* ── Pack N: original-parity HA discovery entities ────────────
     * The original src/MQTTManager.cpp shipped seven more entities that
     * gave Home Assistant first-class control over the device. We add them
     * back here so user dashboards built against AWTRIX3 v2 keep working
     * without modification. */

    /* Helper: HA select / button / switch entities, all built off the same
     * device fragment. */
    auto select = [&](const char* id, const char* name, const char* cmdSfx,
                      const char* cmdTemplate, const std::vector<std::string>& options)
    {
        std::string optsJson;
        optsJson.reserve(options.size() * 8 + 2);
        optsJson += '[';
        for (size_t i = 0; i < options.size(); ++i)
        {
            if (i) optsJson += ',';
            optsJson += '"';
            optsJson += json_escape(options[i]);
            optsJson += '"';
        }
        optsJson += ']';
        pub("select", id, string_format(
                "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"command_topic\":\"%s/%s\","
                "\"command_template\":\"%s\",\"options\":%s,%s}",
                name, objectIdJson.c_str(), id, prefixJson.c_str(), cmdSfx,
                cmdTemplate, optsJson.c_str(), dev.c_str()));
    };

    auto button = [&](const char* id, const char* name, const char* cmdSfx, const char* payload)
    {
        pub("button", id, string_format(
                "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"command_topic\":\"%s/%s\","
                "\"payload_press\":\"%s\",%s}",
                name, objectIdJson.c_str(), id, prefixJson.c_str(), cmdSfx,
                payload ? payload : "", dev.c_str()));
    };

    auto switchEntity = [&](const char* id, const char* name, const char* stateSfx,
                            const char* cmdSfx, const char* onTpl, const char* offTpl)
    {
        pub("switch", id, string_format(
                "{\"name\":\"%s\",\"unique_id\":\"%s_%s\","
                "\"state_topic\":\"%s/%s\","
                "\"command_topic\":\"%s/%s\","
                "\"payload_on\":\"%s\",\"payload_off\":\"%s\","
                "\"state_on\":\"true\",\"state_off\":\"false\",%s}",
                name, objectIdJson.c_str(), id,
                prefixJson.c_str(), stateSfx,
                prefixJson.c_str(), cmdSfx,
                onTpl, offTpl, dev.c_str()));
    };

    /* BriMode: auto vs manual brightness control (original BriMode HASelect).
     * Map: "Auto" → {"ABRI":true},  "Manual" → {"ABRI":false}. */
    select("brimode", "AWTRIX Brightness Mode", "settings",
           "{\\\"ABRI\\\":{{ \\\"true\\\" if value == \\\"Auto\\\" else \\\"false\\\" }}}",
           {"Auto", "Manual"});

    /* TransEffect: transition effect picker — original list of 11 effects
     * matches src/MatrixDisplayUi.cpp transition order. */
    select("trans_effect", "AWTRIX Transition", "settings",
           "{\\\"TEFF\\\":{{ value }}}",
           {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"});

    /* Auto-transition switch: ATRANS true/false. */
    switchEntity("auto_transition", "AWTRIX Auto Transition",
                 "stats/auto_transition", "settings",
                 "{\\\"ATRANS\\\":true}",
                 "{\\\"ATRANS\\\":false}");

    /* Dismiss / next / prev app buttons — original three HAButton entries. */
    button("dismiss", "AWTRIX Dismiss Notification", "notify/dismiss", "");
    button("next_app", "AWTRIX Next App", "nextapp", "");
    button("prev_app", "AWTRIX Previous App", "previousapp", "");

    /* myOwnID sensor — exposes CONFIG.uniqueID so HA can correlate the device
     * with its public identifier. */
    pub("sensor", "myOwnID", string_format(
            "{\"name\":\"AWTRIX Own ID\",\"unique_id\":\"%s_ownid\",\"state_topic\":\"%s/stats/uniqueid\",%s}",
            objectIdJson.c_str(), prefixJson.c_str(), dev.c_str()));

    /* One-shot publish so the HA sensor has a non-empty value right after
     * discovery (the runtime stats publisher does not include uniqueID). */
    {
        std::string uniqueId;
        {
            AwtrixConfig::Guard guard(CONFIG);
            uniqueId = CONFIG.uniqueID;
        }
        awtrix_mqtt_publish("stats/uniqueid", uniqueId.c_str());
    }

    ESP_LOGI(TAG, "HA discovery published (21 entities) under %s", haPrefix.c_str());
}

void awtrix_mqtt_publish_app_loop(const char* app)
{
    awtrix_mqtt_publish("stats/currentApp", app ? app : "");
}
