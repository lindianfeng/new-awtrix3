#include "awtrix_protocol.h"

#include <cJSON.h>
#include <cstring>

static bool set_command(AwtrixCommand &out, AwtrixCommandType type, const char *body) {
    out.type = type;
    out.source = AWTRIX_COMMAND_SOURCE_HTTP;
    out.payload = body ? body : "";
    return true;
}

static bool validate_number_range(cJSON *root, const char *key, double minValue, double maxValue, AwtrixProtocolError &err) {
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item) return true;
    if (!cJSON_IsNumber(item) || item->valuedouble < minValue || item->valuedouble > maxValue) {
        err.code = 400;
        err.message = std::string(key) + " out of range";
        return false;
    }
    return true;
}

static bool validate_bool_if_present(cJSON *root, const char *key, AwtrixProtocolError &err) {
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item) return true;
    if (!cJSON_IsBool(item)) {
        err.code = 400;
        err.message = std::string(key) + " must be boolean";
        return false;
    }
    return true;
}

static bool validate_settings_schema(cJSON *root, AwtrixProtocolError &err) {
    return validate_number_range(root, "BRI", 0, 255, err) &&
           validate_number_range(root, "ABRI", 0, 1, err) &&
           validate_number_range(root, "MATP", 1, 60, err) &&
           validate_number_range(root, "ATRANS", 0, 1, err) &&
           validate_number_range(root, "ATIME", 0, 3600000, err) &&
           validate_number_range(root, "TEFF", 0, 255, err) &&
           validate_number_range(root, "TSPEED", 1, 1000, err) &&
           validate_number_range(root, "TCOL", 0, 0xFFFFFF, err) &&
           validate_number_range(root, "TMODE", 0, 255, err) &&
           validate_number_range(root, "WD", 0, 1, err) &&
           validate_number_range(root, "TMOD", 0, 255, err) &&
           validate_number_range(root, "VOL", 0, 30, err) &&
           validate_number_range(root, "GAMMA", 0, 300, err) &&
           validate_number_range(root, "ROT", 0, 1, err) &&
           validate_number_range(root, "MIRROR", 0, 1, err) &&
           validate_number_range(root, "OVERLAY", 0, 7, err) &&
           validate_number_range(root, "EFFECT", -1, 255, err);
}

static bool validate_moodlight_schema(cJSON *root, AwtrixProtocolError &err) {
    return validate_bool_if_present(root, "state", err) &&
           validate_number_range(root, "kelvin", 1000, 40000, err) &&
           validate_number_range(root, "brightness", 0, 255, err);
}

static bool validate_indicator_schema(cJSON *root, AwtrixProtocolError &err) {
    return validate_bool_if_present(root, "state", err) &&
           validate_number_range(root, "blink", 0, 65535, err) &&
           validate_number_range(root, "fade", 0, 65535, err) &&
           validate_number_range(root, "color", 0, 0xFFFFFF, err);
}

bool awtrix_protocol_http_command(const char *path, const char *body,
                                  AwtrixCommand &out, AwtrixProtocolError &err) {
    if (!path) {
        err.code = 400;
        err.message = "missing path";
        return false;
    }

    if (strcmp(path, "/api/power") == 0) return set_command(out, AwtrixCommandType::Power, body);
    if (strcmp(path, "/api/sleep") == 0) return set_command(out, AwtrixCommandType::Sleep, body);
    if (strcmp(path, "/api/notify") == 0) return set_command(out, AwtrixCommandType::Notify, body);
    if (strcmp(path, "/api/notify/dismiss") == 0) return set_command(out, AwtrixCommandType::DismissNotify, body);
    if (strcmp(path, "/api/apps") == 0) return set_command(out, AwtrixCommandType::UpdateApps, body);
    if (strcmp(path, "/api/switch") == 0) return set_command(out, AwtrixCommandType::SwitchApp, body);
    if (strcmp(path, "/api/nextapp") == 0) return set_command(out, AwtrixCommandType::NextApp, body);
    if (strcmp(path, "/api/previousapp") == 0) return set_command(out, AwtrixCommandType::PreviousApp, body);
    if (strcmp(path, "/api/settings") == 0) return set_command(out, AwtrixCommandType::Settings, body);
    if (strcmp(path, "/api/moodlight") == 0) return set_command(out, AwtrixCommandType::Moodlight, body);
    if (strcmp(path, "/api/reorder") == 0) return set_command(out, AwtrixCommandType::ReorderApps, body);
    if (strcmp(path, "/api/custom") == 0) return set_command(out, AwtrixCommandType::CustomApp, body);

    if (strcmp(path, "/api/indicator1") == 0 ||
        strcmp(path, "/api/indicator2") == 0 ||
        strcmp(path, "/api/indicator3") == 0) {
        set_command(out, AwtrixCommandType::Indicator, body);
        out.index = path[strlen(path) - 1] - '0';
        return true;
    }

    err.code = 404;
    err.message = "unsupported command path";
    return false;
}

bool awtrix_protocol_parse_http(const char *path, const char *body,
                                AwtrixCommand &out, AwtrixProtocolError &err) {
    return awtrix_protocol_http_command(path, body, out, err);
}

bool awtrix_protocol_parse_mqtt(const char *topic, const char *payload, AwtrixCommand &out, AwtrixProtocolError &err) {
    if (!topic) {
        err.code = 400;
        err.message = "missing topic";
        return false;
    }

    out.source = AWTRIX_COMMAND_SOURCE_MQTT;
    out.payload = payload ? payload : "";

    if (strcmp(topic, "notify") == 0) { out.type = AwtrixCommandType::Notify; return true; }
    if (strcmp(topic, "notify/dismiss") == 0) { out.type = AwtrixCommandType::DismissNotify; return true; }
    if (strcmp(topic, "apps") == 0) { out.type = AwtrixCommandType::UpdateApps; return true; }
    if (strcmp(topic, "switch") == 0) { out.type = AwtrixCommandType::SwitchApp; return true; }
    if (strcmp(topic, "settings") == 0) { out.type = AwtrixCommandType::Settings; return true; }
    if (strcmp(topic, "nextapp") == 0) { out.type = AwtrixCommandType::NextApp; return true; }
    if (strcmp(topic, "previousapp") == 0) { out.type = AwtrixCommandType::PreviousApp; return true; }
    if (strcmp(topic, "power") == 0) { out.type = AwtrixCommandType::Power; return true; }
    if (strcmp(topic, "sleep") == 0) { out.type = AwtrixCommandType::Sleep; return true; }
    if (strcmp(topic, "moodlight") == 0) { out.type = AwtrixCommandType::Moodlight; return true; }
    if (strcmp(topic, "sound") == 0) { out.type = AwtrixCommandType::Sound; return true; }
    if (strcmp(topic, "r2d2") == 0) { out.type = AwtrixCommandType::R2D2; return true; }
    if (strcmp(topic, "rtttl") == 0) { out.type = AwtrixCommandType::Rtttl; return true; }
    if (strcmp(topic, "reboot") == 0) { out.type = AwtrixCommandType::Reboot; return true; }

    if (strcmp(topic, "indicator1") == 0 || strcmp(topic, "indicator2") == 0 || strcmp(topic, "indicator3") == 0) {
        out.type = AwtrixCommandType::Indicator;
        out.index = topic[strlen(topic) - 1] - '0';
        return true;
    }

    const char *customPrefix = "custom/";
    const size_t customPrefixLen = strlen(customPrefix);
    if (strncmp(topic, customPrefix, customPrefixLen) == 0 && topic[customPrefixLen] != '\0') {
        out.type = AwtrixCommandType::CustomApp;
        out.name = topic + customPrefixLen;
        return true;
    }

    err.code = 404;
    err.message = "unsupported mqtt topic";
    return false;
}

bool awtrix_protocol_validate_json_object(const char *body, bool allowEmpty, AwtrixProtocolError &err) {
    if (!body || !*body) {
        if (allowEmpty) return true;
        err.code = 400;
        err.message = "empty json body";
        return false;
    }

    cJSON *doc = cJSON_Parse(body);
    if (!doc) {
        err.code = 400;
        err.message = "invalid json";
        return false;
    }
    const bool ok = cJSON_IsObject(doc);
    cJSON_Delete(doc);
    if (!ok) {
        err.code = 400;
        err.message = "json object expected";
        return false;
    }
    return true;
}

static bool validate_command_body(const AwtrixCommand &command, const char *body, AwtrixProtocolError &err) {
    if (!awtrix_protocol_validate_json_object(body, true, err)) return false;
    if (!body || !*body) return true;

    cJSON *doc = cJSON_Parse(body);
    if (!doc) {
        err.code = 400;
        err.message = "invalid json";
        return false;
    }

    bool ok = true;
    switch (command.type) {
        case AwtrixCommandType::Settings:
            ok = validate_settings_schema(doc, err);
            break;
        case AwtrixCommandType::Moodlight:
            ok = validate_moodlight_schema(doc, err);
            break;
        case AwtrixCommandType::Indicator:
            ok = validate_indicator_schema(doc, err);
            break;
        default:
            ok = true;
            break;
    }
    cJSON_Delete(doc);
    return ok;
}

bool awtrix_protocol_validate_http_body(const char *path, const char *body, AwtrixProtocolError &err) {
    AwtrixCommand command;
    if (!awtrix_protocol_http_command(path, body, command, err)) return false;
    return validate_command_body(command, body, err);
}

bool awtrix_protocol_validate_mqtt_body(const char *topic, const char *body, AwtrixProtocolError &err) {
    AwtrixCommand command;
    if (!awtrix_protocol_parse_mqtt(topic, body, command, err)) return false;
    return validate_command_body(command, body, err);
}
