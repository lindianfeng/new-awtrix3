#include "awtrix_globals.h"
#include "awtrix_utils.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string.h>
#include <cJSON.h>
#include "esp_log.h"

#define TAG TAG_SYSTEM

/* All NVS access goes through awtrix_utils.h wrappers */

/* ── singleton ─────────────────────────────────────────────────── */
AwtrixConfig::AwtrixConfig() { setDefaults(); }

void AwtrixConfig::setDefaults() {
    version     = AWTRIX_VERSION;
    hostname    = "awtrix";
    net_ip      = "192.168.178.10";
    net_gw      = "192.168.178.1";
    net_sn      = "255.255.255.0";
    net_pdns    = "223.5.5.5";
    net_sdns    = "180.76.76.76";
    net_static  = false;
    ap_mode     = false;

    mqtt_host  = "";
    mqtt_port  = 1883;
    mqtt_user  = "";
    mqtt_pass  = "";
    mqtt_prefix= "";
    ha_discovery = false;
    ha_prefix    = "homeassistant";
    io_broker    = false;

    auth_user  = "";
    auth_pass  = "";
    web_port   = 80;
    ap_timeout = 300;
    debug_mode = true;

    ntp_server = "pool.ntp.org";
    ntp_tz     = "CET-1CEST,M3.5.0,M10.5.0/3";

    brightness       = 120;
    auto_brightness  = false;
    min_brightness   = 10;
    max_brightness   = 255;
    ldr_factor       = 1.0f;
    ldr_gamma        = 1.0f;
    ldr_on_ground    = false;

    matrix_fps       = 42;
    matrix_layout    = 1;
    mirror_display   = false;
    rotate_screen    = false;
    matrix_off       = false;
    uppercase_letters = true;
    auto_transition  = false;
    block_navigation = false;
    swap_buttons     = false;
    scroll_speed     = 100;

    textColor888     = 0xFFFFFF;
    timeColor = dateColor = tempColor = humColor = batColor = 0;
    calendarHeader   = 0xFF0000;
    calendarText     = 0x000000;
    calendarBody     = 0xFFFFFF;
    wdcActive        = 0xFFFFFF;
    wdcInactive      = 0x666666;
    gamma            = 0; /* 0 means no gamma */

    timeFormat       = "%H:%M:%S";
    dateFormat       = "%d.%m.%y";
    timeMode         = 1;
    showSeconds      = true;
    showWeekday      = true;
    startOnMonday    = true;

    showTime = true;
    showDate = showTemp = showHum = showBat = showWeather = false;

    transEffect       = 0;
    timePerTransition = 400;
    timePerApp        = 7000;

    tempSensorType    = 0; /* TEMP_SENSOR_TYPE_NONE */
    tempOffset        = -9;
    humOffset         = 0;
    tempDecimalPlaces = 0;
    sensorReading     = true;
    isCelsius         = true;

    minBattery = 475;
    maxBattery = 665;

    soundActive       = true;
    soundVolume       = 25;
    bootSound         = "";
    buzzerVolume      = false;

    bgEffect          = BG_NONE;
    globalOverlay     = OVERLAY_NONE;

    statsInterval     = 10000;
    newYear           = false;
    buttonCallback    = "";

    /* runtime (non-persisted) */
    currentTemp = currentHum = currentLux = 0.0f;
    ldrRaw = batteryRaw = 0;
    batteryPercent = 0;
    currentApp = "";
    artnetMode = moodlightMode = false;
    receivedMessages = 0;
    updateAvailable  = false;
    sensorsStable    = false;
    gameActive       = false;

    colorCorrection  = CRGB(255, 255, 255);
    colorTemperature = CRGB(255, 255, 255);
}

/* ── load from NVS ───────────────────────────────────────────── */
void AwtrixConfig::load() {
    SETTINGS_BEGIN();

    brightness       = SETTINGS_GET_U8("BRI", 120);
    auto_brightness  = SETTINGS_GET_BOOL("ABRI", false);
    uppercase_letters= SETTINGS_GET_BOOL("UPPER", true);
    textColor888     = SETTINGS_GET_U32("TCOL", 0xFFFFFF);
    timeColor        = SETTINGS_GET_U32("TIMECOL", 0);
    dateColor        = SETTINGS_GET_U32("DATECOL", 0);
    tempColor        = SETTINGS_GET_U32("TEMPCOL", 0);
    humColor         = SETTINGS_GET_U32("HUMCOL", 0);
    batColor         = SETTINGS_GET_U32("BATCOL", 0);
    calendarHeader   = SETTINGS_GET_U32("CHCOL", 0xFF0000);
    calendarText     = SETTINGS_GET_U32("CTCOL", 0x000000);
    calendarBody     = SETTINGS_GET_U32("CBCOL", 0xFFFFFF);
    wdcActive        = SETTINGS_GET_U32("WDCA", 0xFFFFFF);
    wdcInactive      = SETTINGS_GET_U32("WDCI", 0x666666);
    transEffect      = SETTINGS_GET_U8("TEFF", 0);
    timeMode         = SETTINGS_GET_U8("TMODE", 1);
    timePerTransition= SETTINGS_GET_U32("TSPEED", 400);
    timePerApp       = (long)SETTINGS_GET_U32("ATIME", 7000);
    scroll_speed     = SETTINGS_GET_U8("SSPEED", 100);
    matrix_layout    = SETTINGS_GET_U8("MAT", 1);
    auto_transition  = SETTINGS_GET_BOOL("ATRANS", true);
    block_navigation = SETTINGS_GET_BOOL("BLOCKN", false);
    showWeekday       = SETTINGS_GET_BOOL("WD", true);
    startOnMonday    = SETTINGS_GET_BOOL("SOM", true);
    isCelsius        = SETTINGS_GET_BOOL("CEL", true);
    showTime         = SETTINGS_GET_BOOL("TIM", true);
    showDate         = SETTINGS_GET_BOOL("DAT", false);
    showTemp         = SETTINGS_GET_BOOL("TEMP", false);
    showHum          = SETTINGS_GET_BOOL("HUM", false);
    showBat          = SETTINGS_GET_BOOL("BAT", false);
    soundActive      = SETTINGS_GET_BOOL("SOUND", true);
    soundVolume      = SETTINGS_GET_U8("VOL", 25);

    char buf[128];
    awtrix_settings_load_str("TFORMAT", buf, sizeof(buf), "%H %M");
    timeFormat = buf;
    awtrix_settings_load_str("DFORMAT", buf, sizeof(buf), "%m.%d %w");
    dateFormat = buf;

    // Load dev settings from SPIFFS /dev.json
    loadDevSettings();

    mqtt_prefix = uniqueID.empty() ? "awtrix" : uniqueID;
    hostname    = uniqueID.empty() ? "awtrix" : uniqueID;
}

void AwtrixConfig::loadDevSettings() {
    /* Step A: if /spiffs/dev.json exists, parse it and stage every key into
     * NVS so subsequent boots use the persisted values. Mirrors the original
     * src/Globals.cpp loadDevSettings(): the file is a one-shot bootstrap. */
    FILE *fp = fopen("/spiffs/dev.json", "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        rewind(fp);
        if (sz > 0 && sz < 16 * 1024) {
            char *jbuf = (char *)malloc(sz + 1);
            if (jbuf) {
                fread(jbuf, 1, sz, fp);
                jbuf[sz] = '\0';
                cJSON *doc = cJSON_Parse(jbuf);
                if (doc) {
                    cJSON *v;
                    /* network */
                    if ((v = cJSON_GetObjectItem(doc, "NET_IP")) && cJSON_IsString(v))
                        awtrix_settings_save_str("IP", v->valuestring);
                    if ((v = cJSON_GetObjectItem(doc, "NET_GW")) && cJSON_IsString(v))
                        awtrix_settings_save_str("GW", v->valuestring);
                    if ((v = cJSON_GetObjectItem(doc, "NET_SN")) && cJSON_IsString(v))
                        awtrix_settings_save_str("SN", v->valuestring);
                    if ((v = cJSON_GetObjectItem(doc, "NET_PDNS")) && cJSON_IsString(v))
                        awtrix_settings_save_str("PDNS", v->valuestring);
                    if ((v = cJSON_GetObjectItem(doc, "NET_STATIC")) && cJSON_IsBool(v))
                        awtrix_settings_save_u8("NS", cJSON_IsTrue(v) ? 1 : 0);
                    /* mqtt */
                    if ((v = cJSON_GetObjectItem(doc, "MQTT_HOST")) && cJSON_IsString(v))
                        awtrix_settings_save_str("MQH", v->valuestring);
                    if ((v = cJSON_GetObjectItem(doc, "MQTT_USER")) && cJSON_IsString(v))
                        awtrix_settings_save_str("MQU", v->valuestring);
                    if ((v = cJSON_GetObjectItem(doc, "MQTT_PASS")) && cJSON_IsString(v))
                        awtrix_settings_save_str("MQP", v->valuestring);
                    if ((v = cJSON_GetObjectItem(doc, "MQTT_PORT")) && cJSON_IsNumber(v))
                        awtrix_settings_save_u16("MQPN", (uint16_t)v->valueint);
                    if ((v = cJSON_GetObjectItem(doc, "HA_DISCOVERY")) && cJSON_IsBool(v))
                        awtrix_settings_save_u8("HA", cJSON_IsTrue(v) ? 1 : 0);
                    if ((v = cJSON_GetObjectItem(doc, "HA_PREFIX")) && cJSON_IsString(v))
                        awtrix_settings_save_str("HAP", v->valuestring);
                    /* misc */
                    if ((v = cJSON_GetObjectItem(doc, "NTP_SERVER")) && cJSON_IsString(v))
                        awtrix_settings_save_str("NTP", v->valuestring);
                    if ((v = cJSON_GetObjectItem(doc, "NTP_TZ")) && cJSON_IsString(v))
                        awtrix_settings_save_str("NTZ", v->valuestring);
                    if ((v = cJSON_GetObjectItem(doc, "HOSTNAME")) && cJSON_IsString(v))
                        awtrix_settings_save_str("BNME", v->valuestring);
                    if ((v = cJSON_GetObjectItem(doc, "BOOT_SOUND")) && cJSON_IsString(v))
                        awtrix_settings_save_str("BTSND", v->valuestring);
                    cJSON_Delete(doc);
                }
                free(jbuf);
            }
        }
        fclose(fp);
        /* one-shot: rename to dev.json.applied so we don't re-apply on next boot */
        rename("/spiffs/dev.json", "/spiffs/dev.json.applied");
        ESP_LOGI(TAG, "dev.json applied to NVS");
    }

    /* Step B: load all keys from NVS (dev.json values now appear here too). */
    char buf[64];
    awtrix_settings_load_str("NTP",  buf, sizeof(buf), "pool.ntp.org");
    awtrix_settings_load_str("NTZ",  buf, sizeof(buf), NTP_TZ_DEFAULT);
    awtrix_settings_load_str("IP",   buf, sizeof(buf), "192.168.178.10");
    awtrix_settings_load_str("GW",   buf, sizeof(buf), "192.168.178.1");
    awtrix_settings_load_str("SN",   buf, sizeof(buf), "255.255.255.0");
    awtrix_settings_load_str("PDNS", buf, sizeof(buf), "223.5.5.5");
    awtrix_settings_load_str("SDNS", buf, sizeof(buf), "180.76.76.76");
    awtrix_settings_load_str("MQH",  buf, sizeof(buf), "");
    awtrix_settings_load_str("MQU",  buf, sizeof(buf), "");
    awtrix_settings_load_str("MQP",  buf, sizeof(buf), "");
    awtrix_settings_load_str("HAP",  buf, sizeof(buf), "homeassistant");
    awtrix_settings_load_str("AUS",  buf, sizeof(buf), "");
    awtrix_settings_load_str("AUP",  buf, sizeof(buf), "");
    awtrix_settings_load_str("BSCB", buf, sizeof(buf), "");
    awtrix_settings_load_str("BNME", buf, sizeof(buf), "awtrix");
    awtrix_settings_load_str("BTSND",buf, sizeof(buf), "");

    /* numeric dev settings */
    /* NOTE: "MQP" is reserved for the MQTT password string above; the port
     * uses an independent key "MQPN" to avoid an NVS type collision. */
    mqtt_port         = awtrix_settings_load_u16("MQPN", 1883);
    ha_discovery      = awtrix_settings_load_u8("HA", 0) != 0;
    io_broker         = awtrix_settings_load_u8("IOB", 0) != 0;
    net_static        = awtrix_settings_load_u8("NS", 0)  != 0;
    sensorReading     = awtrix_settings_load_u8("SR", 1)  != 0;
    mirror_display    = awtrix_settings_load_u8("MS", 0)  != 0;
    tempOffset        = awtrix_settings_load_float("TO", -9);
    minBattery        = awtrix_settings_load_u16("MBAT", 475);
    maxBattery        = awtrix_settings_load_u16("XBT", 665);
    ap_timeout        = awtrix_settings_load_u32("APTMO", 300);
    humOffset         = awtrix_settings_load_float("HO", 0);
    statsInterval     = (long)awtrix_settings_load_u32("SI", 10000);
    web_port          = awtrix_settings_load_u16("WP", 80);
    buzzerVolume      = awtrix_settings_load_u8("BVO", 0) != 0;
    tempDecimalPlaces = awtrix_settings_load_u8("TDP", 0);
    rotate_screen     = awtrix_settings_load_u8("RS", 0)   != 0;
    debug_mode        = awtrix_settings_load_u8("DM", 1)   != 0;
    newYear           = awtrix_settings_load_u8("NY", 0)   != 0;
    swap_buttons      = awtrix_settings_load_u8("SB", 0)   != 0;
    ldr_on_ground     = awtrix_settings_load_u8("LOG", 0)  != 0;
    min_brightness    = awtrix_settings_load_u8("MB", 10);
    max_brightness    = awtrix_settings_load_u8("XB", 255);
    ldr_factor        = awtrix_settings_load_float("LFRC", 1.0f);
    ldr_gamma         = awtrix_settings_load_float("LGAM", 1.0f);
    bgEffect          = (int)awtrix_settings_load_u8("BGE", (uint8_t)BG_NONE);

    uint8_t corr[3]; size_t s = sizeof(corr);
    if (awtrix_settings_load_blob("CC", corr, &s) && s == 3)
        colorCorrection = CRGB(corr[0], corr[1], corr[2]);
    s = sizeof(corr);
    if (awtrix_settings_load_blob("CT", corr, &s) && s == 3)
        colorTemperature = CRGB(corr[0], corr[1], corr[2]);

    /* load string dev settings */
    awtrix_settings_load_str("BTSND", buf, sizeof(buf), "");
    bootSound = buf;
    awtrix_settings_load_str("BSCB", buf, sizeof(buf), "");
    buttonCallback = buf;
}

/* ── save to NVS ─────────────────────────────────────────────── */
void AwtrixConfig::save() {
    SETTINGS_BEGIN_P();
    awtrix_settings_save_u8("BRI",  (uint8_t)brightness);
    awtrix_settings_save_u8("ABRI", auto_brightness ? 1 : 0);
    awtrix_settings_save_u8("UPPER",uppercase_letters ? 1 : 0);
    awtrix_settings_save_u32("TCOL", textColor888);
    awtrix_settings_save_u32("TIMECOL", timeColor);
    awtrix_settings_save_u32("DATECOL", dateColor);
    awtrix_settings_save_u32("TEMPCOL", tempColor);
    awtrix_settings_save_u32("HUMCOL",  humColor);
    awtrix_settings_save_u32("BATCOL",  batColor);
    awtrix_settings_save_u32("CHCOL",  calendarHeader);
    awtrix_settings_save_u32("CTCOL",  calendarText);
    awtrix_settings_save_u32("CBCOL",  calendarBody);
    awtrix_settings_save_u32("WDCA",  wdcActive);
    awtrix_settings_save_u32("WDCI",  wdcInactive);
    awtrix_settings_save_u8("TEFF",  (uint8_t)transEffect);
    awtrix_settings_save_u8("TMODE", (uint8_t)timeMode);
    awtrix_settings_save_u32("TSPEED", (uint32_t)timePerTransition);
    awtrix_settings_save_u32("ATIME",  (uint32_t)timePerApp);
    awtrix_settings_save_u8("SSPEED", (uint8_t)scroll_speed);
    awtrix_settings_save_u8("MAT",   (uint8_t)matrix_layout);
    awtrix_settings_save_u8("ATRANS", auto_transition ? 1 : 0);
    awtrix_settings_save_u8("BLOCKN", block_navigation ? 1 : 0);
    awtrix_settings_save_u8("WD",     showWeekday ? 1 : 0);
    awtrix_settings_save_u8("SOM",    startOnMonday ? 1 : 0);
    awtrix_settings_save_u8("CEL",    isCelsius ? 1 : 0);
    awtrix_settings_save_u8("TIM",    showTime ? 1 : 0);
    awtrix_settings_save_u8("DAT",    showDate ? 1 : 0);
    awtrix_settings_save_u8("TEMP",   showTemp ? 1 : 0);
    awtrix_settings_save_u8("HUM",    showHum ? 1 : 0);
    awtrix_settings_save_u8("BAT",    showBat ? 1 : 0);
    awtrix_settings_save_u8("SOUND",  soundActive ? 1 : 0);
    awtrix_settings_save_u8("VOL",    soundVolume);
    awtrix_settings_save_str("TFORMAT", timeFormat.c_str());
    awtrix_settings_save_str("DFORMAT", dateFormat.c_str());
    awtrix_settings_save_u8("BGE",   (uint8_t)bgEffect);
    awtrix_settings_save_u8("MB",    min_brightness);
    awtrix_settings_save_u8("XB",    max_brightness);
    awtrix_settings_save_u8("DM",    debug_mode ? 1 : 0);
    awtrix_settings_save_u8("RS",    rotate_screen ? 1 : 0);
    awtrix_settings_save_u8("SB",    swap_buttons ? 1 : 0);
}

void AwtrixConfig::eraseAll() {
    awtrix_settings_erase_all();
    setDefaults();
}

/* ── JSON export ─────────────────────────────────────────────── */
std::string AwtrixConfig::toJson() const {
    cJSON *root = cJSON_CreateObject();

    /* identity */
    cJSON_AddStringToObject(root, "VERSION",   version ? version : "");
    cJSON_AddStringToObject(root, "HOSTNAME",  hostname.c_str());

    /* display brightness */
    cJSON_AddNumberToObject(root, "BRI",       brightness);
    cJSON_AddBoolToObject(  root, "ABRI",      auto_brightness);
    cJSON_AddNumberToObject(root, "MB",        min_brightness);
    cJSON_AddNumberToObject(root, "XB",        max_brightness);

    /* matrix / transition */
    cJSON_AddBoolToObject(  root, "UPPER",     uppercase_letters);
    cJSON_AddNumberToObject(root, "TEFF",      transEffect);
    cJSON_AddBoolToObject(  root, "ATRANS",    auto_transition);
    cJSON_AddNumberToObject(root, "ATIME",     (double)timePerApp);
    cJSON_AddNumberToObject(root, "TSPEED",    timePerTransition);
    cJSON_AddNumberToObject(root, "MAT",       matrix_layout);
    cJSON_AddNumberToObject(root, "SSPEED",    scroll_speed);
    cJSON_AddBoolToObject(  root, "BLOCKN",    block_navigation);
    cJSON_AddBoolToObject(  root, "RS",        rotate_screen);
    cJSON_AddBoolToObject(  root, "MS",        mirror_display);
    cJSON_AddBoolToObject(  root, "SB",        swap_buttons);
    cJSON_AddNumberToObject(root, "BGE",       bgEffect);

    /* colors */
    cJSON_AddNumberToObject(root, "TCOL",      textColor888);
    cJSON_AddNumberToObject(root, "TIMECOL",   timeColor);
    cJSON_AddNumberToObject(root, "DATECOL",   dateColor);
    cJSON_AddNumberToObject(root, "TEMPCOL",   tempColor);
    cJSON_AddNumberToObject(root, "HUMCOL",    humColor);
    cJSON_AddNumberToObject(root, "BATCOL",    batColor);
    cJSON_AddNumberToObject(root, "CHCOL",     calendarHeader);
    cJSON_AddNumberToObject(root, "CTCOL",     calendarText);
    cJSON_AddNumberToObject(root, "CBCOL",     calendarBody);
    cJSON_AddNumberToObject(root, "WDCA",      wdcActive);
    cJSON_AddNumberToObject(root, "WDCI",      wdcInactive);

    /* time / date */
    cJSON_AddStringToObject(root, "TFORMAT",   timeFormat.c_str());
    cJSON_AddStringToObject(root, "DFORMAT",   dateFormat.c_str());
    cJSON_AddNumberToObject(root, "TMODE",     timeMode);
    cJSON_AddBoolToObject(  root, "WD",        showWeekday);
    cJSON_AddBoolToObject(  root, "SOM",       startOnMonday);
    cJSON_AddBoolToObject(  root, "CEL",       isCelsius);

    /* native apps visibility */
    cJSON_AddBoolToObject(  root, "TIM",       showTime);
    cJSON_AddBoolToObject(  root, "DAT",       showDate);
    cJSON_AddBoolToObject(  root, "TEMP",      showTemp);
    cJSON_AddBoolToObject(  root, "HUM",       showHum);
    cJSON_AddBoolToObject(  root, "BAT",       showBat);

    /* sensors */
    cJSON_AddNumberToObject(root, "TDP",       tempDecimalPlaces);
    cJSON_AddBoolToObject(  root, "SR",        sensorReading);
    cJSON_AddNumberToObject(root, "TO",        tempOffset);
    cJSON_AddNumberToObject(root, "HO",        humOffset);

    /* audio */
    cJSON_AddBoolToObject(  root, "SOUND",     soundActive);
    cJSON_AddNumberToObject(root, "VOL",       soundVolume);
    cJSON_AddBoolToObject(  root, "BVO",       buzzerVolume);

    /* runtime telemetry (read-only fields useful to UI) */
    cJSON_AddNumberToObject(root, "TEMP_VAL",  currentTemp);
    cJSON_AddNumberToObject(root, "HUM_VAL",   currentHum);
    cJSON_AddNumberToObject(root, "LUX",       currentLux);
    cJSON_AddNumberToObject(root, "BAT_VAL",   batteryPercent);
    cJSON_AddNumberToObject(root, "BAT_RAW",   batteryRaw);
    cJSON_AddNumberToObject(root, "LDR_RAW",   ldrRaw);
    cJSON_AddBoolToObject(  root, "AP",        ap_mode);

    char *s = cJSON_PrintUnformatted(root);
    std::string result(s ? s : "{}");
    if (s) cJSON_free(s);
    cJSON_Delete(root);
    return result;
}
