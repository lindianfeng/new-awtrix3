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
#include "DisplayManager.h"
#include "awtrix_periphery.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"             /* esp_wifi_sta_get_ap_info() */
#include "esp_system.h"           /* esp_get_free_heap_size() */
#include <string>
#include <vector>
#include <cstring>

#define TAG TAG_MQTT

static esp_mqtt_client_handle_t s_client = nullptr;
static bool s_connected = false;
static std::string s_prefix;
static uint64_t s_lastStats = 0;
static std::vector<std::string> s_pending_subs;

static std::string make_topic(const char *suffix) {
    std::string t = s_prefix;
    if (suffix && suffix[0] != '/') t += '/';
    t += suffix ? suffix : "";
    return t;
}

/* ── inbound message dispatcher (replays MQTTManager processMqttMessage) */
static void process_message(const std::string &topic, const std::string &payload) {
    auto &dm  = DisplayManager::get();
    auto &per = PeripheryManager::get();

    auto starts_with = [&](const std::string &t, const std::string &p) {
        return t.size() >= p.size() && t.compare(0, p.size(), p) == 0;
    };
    auto equals = [&](const std::string &t, const char *sfx) {
        std::string full = make_topic(sfx);
        return t == full;
    };

    CONFIG.receivedMessages++;
    if (equals(topic, "notify"))            dm.generateNotification(0, payload.c_str());
    else if (equals(topic, "notify/dismiss")) dm.dismissNotify();
    else if (equals(topic, "apps"))         dm.updateAppVector(payload.c_str());
    else if (equals(topic, "switch"))       dm.switchToApp(payload.c_str());
    else if (equals(topic, "sendscreen"))   awtrix_mqtt_publish("screen", dm.ledsAsJson().c_str());
    else if (equals(topic, "settings"))     dm.setNewSettings(payload.c_str());
    else if (equals(topic, "r2d2"))         per.r2d2(payload.c_str());
    else if (equals(topic, "nextapp"))      dm.nextApp();
    else if (equals(topic, "previousapp"))  dm.previousApp();
    else if (equals(topic, "rtttl"))        per.playRTTTL(payload.c_str());
    else if (equals(topic, "power"))        dm.powerStateParse(payload.c_str());
    else if (equals(topic, "sleep"))        { dm.showSleepAnimation(); /* deep-sleep done by PowerManager */ }
    else if (equals(topic, "indicator1"))   dm.indicatorParser(1, payload.c_str());
    else if (equals(topic, "indicator2"))   dm.indicatorParser(2, payload.c_str());
    else if (equals(topic, "indicator3"))   dm.indicatorParser(3, payload.c_str());
    else if (equals(topic, "moodlight"))    dm.moodlight(payload.c_str());
    else if (equals(topic, "reboot"))       esp_restart();
    else if (equals(topic, "sound"))        per.parseSound(payload.c_str());
    else if (starts_with(topic, make_topic("custom"))) {
        /* "<prefix>/custom/<name>" → name = trailing segment */
        const std::string prefix = make_topic("custom/");
        if (topic.size() > prefix.size())
            dm.parseCustomPage(topic.c_str() + prefix.size(), payload.c_str(), false);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data) {
    (void)handler_args; (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "MQTT connected");
        /* core subscriptions */
        {
            const char *core[] = {
                "notify","notify/dismiss","apps","switch","sendscreen",
                "settings","r2d2","nextapp","previousapp","rtttl","power","sleep",
                "indicator1","indicator2","indicator3","moodlight","reboot","sound",
                "custom/+",
            };
            for (auto t : core) {
                std::string full = make_topic(t);
                esp_mqtt_client_subscribe(s_client, full.c_str(), 0);
            }
            for (auto &t : s_pending_subs) esp_mqtt_client_subscribe(s_client, t.c_str(), 0);
        }
        /* HA discovery (optional) */
        if (CONFIG.ha_discovery) awtrix_mqtt_publish_ha_discovery();
        /* presence */
        awtrix_mqtt_publish("stats/version", CONFIG.version ? CONFIG.version : "");
        awtrix_mqtt_publish("stats/effects", DisplayManager::get().getEffectNames().c_str());
        awtrix_mqtt_publish("stats/transitions", DisplayManager::get().getTransitionNames().c_str());
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_DATA: {
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

void awtrix_mqtt_init(void) {
    auto &c = CONFIG;
    if (c.mqtt_host.empty()) {
        ESP_LOGI(TAG, "MQTT disabled (no host configured)");
        return;
    }
    s_prefix = c.mqtt_prefix.empty() ? std::string("awtrix") : c.mqtt_prefix;

    char uri[160];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", c.mqtt_host.c_str(), c.mqtt_port);

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = uri;
    if (!c.mqtt_user.empty()) cfg.credentials.username = c.mqtt_user.c_str();
    if (!c.mqtt_pass.empty()) cfg.credentials.authentication.password = c.mqtt_pass.c_str();
    cfg.credentials.client_id = c.hostname.c_str();
    cfg.session.keepalive = 30;
    cfg.network.disable_auto_reconnect = false;
    cfg.network.reconnect_timeout_ms = 5000;

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) { ESP_LOGE(TAG, "MQTT client init failed"); return; }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_event_handler, nullptr);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "MQTT started → %s", uri);
}

void awtrix_mqtt_tick(void) {
    if (!s_client || !s_connected) return;
    uint64_t now = esp_timer_get_time() / 1000;
    if (now - s_lastStats >= (uint64_t)CONFIG.statsInterval) {
        s_lastStats = now;
        auto &c = CONFIG;
        char buf[64];
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)(now / 1000));
        awtrix_mqtt_publish("stats/uptime", buf);
        snprintf(buf, sizeof(buf), "%.2f", c.currentTemp);
        awtrix_mqtt_publish("stats/temperature", buf);
        snprintf(buf, sizeof(buf), "%.2f", c.currentHum);
        awtrix_mqtt_publish("stats/humidity", buf);
        snprintf(buf, sizeof(buf), "%u", (unsigned)c.batteryPercent);
        awtrix_mqtt_publish("stats/battery", buf);
        snprintf(buf, sizeof(buf), "%.0f", c.currentLux);
        awtrix_mqtt_publish("stats/lux", buf);

        /* Free heap. */
        snprintf(buf, sizeof(buf), "%u", (unsigned)esp_get_free_heap_size());
        awtrix_mqtt_publish("stats/ram", buf);

        /* Brightness. */
        snprintf(buf, sizeof(buf), "%d", c.brightness);
        awtrix_mqtt_publish("stats/brightness", buf);

        /* WiFi RSSI. */
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(buf, sizeof(buf), "%d", (int)ap.rssi);
            awtrix_mqtt_publish("stats/rssi", buf);
        }

        /* IP address. */
        char ip[40] = {0};
        awtrix_wifi_get_ip_str(ip, sizeof(ip));
        if (ip[0]) awtrix_mqtt_publish("stats/ip", ip);
    }
}

bool awtrix_mqtt_is_connected(void) { return s_connected; }

void awtrix_mqtt_publish(const char *suffix, const char *payload) {
    if (!s_client) return;
    std::string t = make_topic(suffix);
    esp_mqtt_client_publish(s_client, t.c_str(), payload ? payload : "", 0, 0, 0);
}

void awtrix_mqtt_subscribe(const char *suffix) {
    std::string t = make_topic(suffix);
    if (s_client && s_connected) esp_mqtt_client_subscribe(s_client, t.c_str(), 0);
    else s_pending_subs.push_back(t);
}

void awtrix_mqtt_publish_ha_discovery(void) {
    if (!s_client) return;
    auto &c = CONFIG;
    const std::string haPrefix = c.ha_prefix.empty() ? std::string("homeassistant") : c.ha_prefix;
    const std::string host = c.hostname;
    const std::string ver  = c.version ? c.version : "";

    /* Common device fragment reused in every entity payload — tying every
     * entity to one HA device so they appear grouped under "AWTRIX <host>". */
    char devBuf[256];
    snprintf(devBuf, sizeof(devBuf),
        "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"AWTRIX %s\","
        "\"manufacturer\":\"AWTRIX3 ESP-IDF\",\"model\":\"Ulanzi S3\",\"sw_version\":\"%s\"}",
        host.c_str(), host.c_str(), ver.c_str());
    const std::string dev(devBuf);

    auto pub = [&](const char *component, const char *id, const std::string &payload) {
        std::string topic = haPrefix + "/" + component + "/" + host + "/" + id + "/config";
        esp_mqtt_client_publish(s_client, topic.c_str(), payload.c_str(), 0, 0, 1);
    };

    auto sensor = [&](const char *id, const char *name, const char *stateSfx,
                      const char *unit, const char *deviceClass) {
        char buf[640];
        snprintf(buf, sizeof(buf),
            "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"state_topic\":\"%s/%s\","
            "\"unit_of_measurement\":\"%s\"%s%s%s,%s}",
            name, host.c_str(), id, s_prefix.c_str(), stateSfx, unit ? unit : "",
            deviceClass ? ",\"device_class\":\"" : "",
            deviceClass ? deviceClass : "",
            deviceClass ? "\"" : "",
            dev.c_str());
        pub("sensor", id, buf);
    };

    auto binary = [&](const char *id, const char *name, const char *stateSfx) {
        char buf[480];
        snprintf(buf, sizeof(buf),
            "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"state_topic\":\"%s/%s\","
            "\"payload_on\":\"PRESSED\",\"payload_off\":\"RELEASED\",%s}",
            name, host.c_str(), id, s_prefix.c_str(), stateSfx, dev.c_str());
        pub("binary_sensor", id, buf);
    };

    auto indicatorLight = [&](int n) {
        char id[32]; snprintf(id, sizeof(id), "indicator%d", n);
        char buf[640];
        snprintf(buf, sizeof(buf),
            "{\"name\":\"AWTRIX Indicator %d\",\"unique_id\":\"%s_%s\","
            "\"schema\":\"json\",\"command_topic\":\"%s/%s\","
            "\"state_topic\":\"%s/stats/%s\",\"rgb\":true,\"brightness\":false,%s}",
            n, host.c_str(), id, s_prefix.c_str(), id,
            s_prefix.c_str(), id, dev.c_str());
        pub("light", id, buf);
    };

    /* 1) Main matrix as a json-schema light. */
    {
        char buf[700];
        snprintf(buf, sizeof(buf),
            "{\"name\":\"AWTRIX\",\"unique_id\":\"%s_light\",\"schema\":\"json\","
            "\"command_topic\":\"%s/power\",\"state_topic\":\"%s/stats/power\","
            "\"brightness\":true,\"rgb\":true,%s}",
            host.c_str(), s_prefix.c_str(), s_prefix.c_str(), dev.c_str());
        pub("light", "matrix", buf);
    }

    /* 2-7) Telemetry sensors. */
    sensor("temperature", "AWTRIX Temperature", "stats/temperature", "°C",  "temperature");
    sensor("humidity",    "AWTRIX Humidity",    "stats/humidity",    "%",   "humidity");
    sensor("battery",     "AWTRIX Battery",     "stats/battery",     "%",   "battery");
    sensor("lux",         "AWTRIX Lux",         "stats/lux",         "lx",  "illuminance");
    sensor("uptime",      "AWTRIX Uptime",      "stats/uptime",      "s",   "duration");
    sensor("rssi",        "AWTRIX RSSI",        "stats/rssi",        "dBm", "signal_strength");
    {
        char buf[480];
        snprintf(buf, sizeof(buf),
            "{\"name\":\"AWTRIX IP\",\"unique_id\":\"%s_ip\",\"state_topic\":\"%s/stats/ip\",%s}",
            host.c_str(), s_prefix.c_str(), dev.c_str());
        pub("sensor", "ip", buf);
        snprintf(buf, sizeof(buf),
            "{\"name\":\"AWTRIX Version\",\"unique_id\":\"%s_ver\",\"state_topic\":\"%s/stats/version\",%s}",
            host.c_str(), s_prefix.c_str(), dev.c_str());
        pub("sensor", "version", buf);
        snprintf(buf, sizeof(buf),
            "{\"name\":\"AWTRIX Free RAM\",\"unique_id\":\"%s_ram\",\"state_topic\":\"%s/stats/ram\","
            "\"unit_of_measurement\":\"B\",%s}",
            host.c_str(), s_prefix.c_str(), dev.c_str());
        pub("sensor", "ram", buf);
        snprintf(buf, sizeof(buf),
            "{\"name\":\"AWTRIX Current App\",\"unique_id\":\"%s_app\",\"state_topic\":\"%s/stats/currentApp\",%s}",
            host.c_str(), s_prefix.c_str(), dev.c_str());
        pub("sensor", "currentApp", buf);
    }

    /* 8-10) Three indicator LEDs as separate json-schema lights. */
    indicatorLight(1);
    indicatorLight(2);
    indicatorLight(3);

    /* 11-13) Three buttons as binary sensors (PRESSED / RELEASED). */
    binary("button_left",   "AWTRIX Button Left",   "stats/btn_left");
    binary("button_select", "AWTRIX Button Select", "stats/btn_select");
    binary("button_right",  "AWTRIX Button Right",  "stats/btn_right");

    /* 14) Brightness number (0-255). */
    {
        char buf[640];
        snprintf(buf, sizeof(buf),
            "{\"name\":\"AWTRIX Brightness\",\"unique_id\":\"%s_brightness\","
            "\"command_topic\":\"%s/settings\",\"state_topic\":\"%s/stats/brightness\","
            "\"command_template\":\"{\\\"BRI\\\":{{value}}}\","
            "\"min\":0,\"max\":255,\"step\":1,%s}",
            host.c_str(), s_prefix.c_str(), s_prefix.c_str(), dev.c_str());
        pub("number", "brightness", buf);
    }

    ESP_LOGI(TAG, "HA discovery published (14 entities) under %s", haPrefix.c_str());
}

void awtrix_mqtt_publish_app_loop(const char *app) {
    awtrix_mqtt_publish("stats/currentApp", app ? app : "");
}
