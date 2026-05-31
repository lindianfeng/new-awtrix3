/**
 * AWTRIX3 – ESP-IDF port
 * Target: ESP32-S3 (Ulanzi clock hardware)
 *
 * Original AWTRIX3 copyright (C) 2024 Stephan Mühl (Blueforcer)
 * License: Creative Commons Attribution-NonCommercial-ShareAlike 4.0
 */

#include "driver/gpio.h"
#include "awtrix_hal.h"
#include "led_matrix.h"
#include "matrix_cpp.h"
#include "awtrix_storage.h"
#include "awtrix_globals.h"
#include "awtrix_network.h"
#include "awtrix_mqtt.h"
#include "awtrix_artnet.h"
#include "awtrix_http.h"
#include "awtrix_api.h"
#include "DisplayManager.h"
#include "awtrix_periphery.h"
#include "awtrix_events.h"       /* AwtrixEventBus / EVENTS macro */
#include "awtrix_command_bus.h"
#include "effects_core.h"
#include "awtrix_menumanager.h"
#include <string>

static const char *TAG = TAG_SYSTEM;
static Matrix *s_matrix = nullptr;
static AwtrixHttpServer *s_http = nullptr;

/* C-linkage forward declaration for the BUTTON_CALLBACK helper that lives
 * in awtrix_api.cpp; needed at file scope because extern "C" cannot appear
 * inside a function body in C++. */
extern "C" void awtrix_button_callback_fire(int idx, int evt);

/* ── Button → DisplayManager / MenuManager bridge ─────────────── */
static void onButton(int idx, btn_event_t evt) {
    /* Honour the user-toggled swap_buttons: when the device is mounted
     * upside-down the LEFT and RIGHT buttons must logically swap so
     * navigation feels natural. Mirrors the original AWTRIX3 behaviour. */
    if (CONFIG.swap_buttons) {
        if      (idx == BTN_LEFT)  idx = BTN_RIGHT;
        else if (idx == BTN_RIGHT) idx = BTN_LEFT;
    }

    if (idx == BTN_RESET && evt == BTN_EVENT_VERY_LONG_PRESS) {
        ESP_LOGI(TAG, "Factory reset triggered");
        AwtrixConfig::get().eraseAll();
        esp_restart();
    }

    /* Mirror to MQTT binary_sensor for HA. PRESSED / RELEASED states
     * match the discovery payload published by awtrix_mqtt_publish_ha_discovery. */
    if (evt == BTN_EVENT_PRESSED || evt == BTN_EVENT_LONG_PRESS ||
        evt == BTN_EVENT_DOUBLE_PRESS || evt == BTN_EVENT_VERY_LONG_PRESS) {
        const char *topic = nullptr;
        switch (idx) {
            case BTN_LEFT:   topic = "stats/btn_left";   break;
            case BTN_SELECT: topic = "stats/btn_select"; break;
            case BTN_RIGHT:  topic = "stats/btn_right";  break;
            default: break;
        }
        if (topic) {
            awtrix_mqtt_publish(topic, "PRESSED");
            /* Fire the user-configured BUTTON_CALLBACK URL (if any). The
             * extern "C" prototype lives at file scope. */
            awtrix_button_callback_fire(idx, (int)evt);
        }
    }

    if (evt == BTN_EVENT_PRESSED) {
        AwtrixCommand command;
        command.type = AwtrixCommandType::Button;
        command.source = AWTRIX_COMMAND_SOURCE_BUTTON;
        if (idx == BTN_LEFT) command.index = 0;
        else if (idx == BTN_SELECT) command.index = 1;
        else if (idx == BTN_RIGHT) command.index = 2;
        else return;
        if (!awtrix_command_bus_post(command, 0)) ESP_LOGW(TAG, "Dropped button command idx=%d", idx);
    }

    if (idx == BTN_SELECT && evt == BTN_EVENT_LONG_PRESS) {
        AwtrixCommand command;
        command.type = AwtrixCommandType::Button;
        command.source = AWTRIX_COMMAND_SOURCE_BUTTON;
        command.index = 3;
        if (!awtrix_command_bus_post(command, 0)) ESP_LOGW(TAG, "Dropped button long-press command");
    }
}

extern "C" void app_main(void)
{
    /* ── 1. Boot delay ──────────────────────────────────────── */
    gpio_set_direction(GPIO_NUM_15, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_15, 0);
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* ── 2. HAL init ────────────────────────────────────────── */
    awtrix_init();

    /* ── 3. NVS + Config ────────────────────────────────────── */
    awtrix_settings_init();
    auto &cfg = AwtrixConfig::get();
    cfg.load();

    /* Cross-task command bus: HTTP/MQTT/button producers enqueue commands,
     * DisplayManager drains them from the main loop to keep UI state single-writer. */
    awtrix_command_bus_init();

    /* ── 4. SPIFFS ──────────────────────────────────────────── */
    bool fs_ok = awtrix_fs_mount();
    ESP_LOGI(TAG, "FS=%s", fs_ok ? "OK" : "FAIL");

    /* ── 5. Matrix ──────────────────────────────────────────── */
    s_matrix = new Matrix(
        MATRIX_WIDTH, MATRIX_HEIGHT, 4, 1,
        NEO_MATRIX_TOP | NEO_MATRIX_LEFT | NEO_MATRIX_ROWS | MATRIX_TYPE
    );
    if (!s_matrix || !s_matrix->getLeds()) {
        ESP_LOGE(TAG, "Matrix alloc failed — halting");
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    s_matrix->setBrightness(cfg.brightness);
    /* Apply the user-configured 180° rotation immediately so the boot
     * animation and every subsequent frame come out the right way up
     * on an upside-down-mounted Ulanzi clock. */
    if (cfg.rotate_screen) s_matrix->setRotation(2);
    s_matrix->clear();
    s_matrix->show();

    /* ── 6. DisplayManager ──────────────────────────────────── */
    auto &disp = DisplayManager::get();
    disp.setup(s_matrix);
    disp.loadNativeApps();

    /* ── 7. Boot animation ──────────────────────────────────── */
    s_matrix->clear();
    s_matrix->setCursor(2, 1);
    s_matrix->setTextColor(Matrix::Color(0, 200, 0));
    s_matrix->print("AWTRIX");
    s_matrix->setCursor(9, 4);
    s_matrix->setTextColor(Matrix::Color(255, 255, 255));
    s_matrix->print(AWTRIX_VERSION);
    s_matrix->show();

    /* ── 8. Wi-Fi start (non-blocking) ─────────────────────── */
    awtrix_wifi_start();

    /* ── 9. Wait for Wi-Fi with progress animation ──────────── */
    int dot = 0;
    while (!awtrix_wifi_is_ready()) {
        s_matrix->clear();
        s_matrix->setCursor(2, 1);
        s_matrix->setTextColor(Matrix::Color(0, 200, 0));
        s_matrix->print("AWTRIX");
        s_matrix->setCursor(4, 4);
        s_matrix->setTextColor(Matrix::Color(255, 255, 255));
        for (int i = 0; i < 4; i++) {
            s_matrix->print(i <= (dot % 4) ? '.' : ' ');
        }
        s_matrix->show();
        dot++;
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    char ipBuf[32];
    awtrix_wifi_get_ip_str(ipBuf, sizeof(ipBuf));
    if (!awtrix_wifi_is_connected()) cfg.ap_mode = true;

    /* ── 10. SNTP ───────────────────────────────────────────── */
    awtrix_sntp_init(cfg.ntp_server.c_str(), cfg.ntp_tz.c_str());

    /* ── 11. mDNS ───────────────────────────────────────────── */
    awtrix_mdns_init(cfg.hostname.c_str(), "");
    awtrix_mdns_add_service("tcp", "http", cfg.web_port);
    awtrix_mdns_add_service("tcp", "awtrix", cfg.web_port);
    awtrix_udp_discovery_init(4210);

    /* ── 12. Periphery (Phase A — internal setup) ────────────
     * MUST happen before HTTP/MQTT/TCP go live. Otherwise a fast inbound
     * request (or even a physical button press during boot) could hit a
     * nullptr onButtonEvent or an unwired EventBus slot. */
    auto &peri = PeripheryManager::get();
    peri.onButtonEvent = onButton;
    peri.setup();
    peri.playBootSound();

    /* ── 13. Wire the AwtrixEventBus ──────────────────────────
     * Both halves of the formerly cyclic awtrix_core ↔ awtrix_periphery
     * coupling now flow through std::function slots. Wire AFTER both
     * subsystems are alive AND BEFORE any external interface (HTTP/MQTT)
     * can fire an event.
     *
     * E1 (round 8): EventBus carries only immediate-action slots; control
     * flow that used to fan out through here (ShowSleepScreen / SetBrightness
     * / Notify) now goes straight to awtrix_command_bus_post() from the
     * producing module, removing main as a passive trampoline. */
    auto &bus = AwtrixEventBus::get();
    bus.onRtttlAction     = [](const char *r){ if (r) PeripheryManager::get().playRTTTL(r); };
    bus.onSoundAction     = [](const char *j){ PeripheryManager::get().parseSound(j); };
    bus.onR2D2Action      = [](const char *m){ if (m) PeripheryManager::get().r2d2(m); };
    bus.onSetVolumeAction = [](uint8_t v)    { PeripheryManager::get().setVolume(v); };
    ESP_LOGI(TAG, "EventBus wired (4 immediate-action slots to periphery)");

    /* ── 14. Restore persisted custom apps from /spiffs/CUSTOMAPPS ── */
    disp.loadCustomApps();

    /* ── 15. HTTP server (Phase B — start exposing to the world) ── */
    s_http = new AwtrixHttpServer();
    if (s_http->start(cfg.web_port)) {
        awtrix_api_register_routes(*s_http);
        ESP_LOGI(TAG, "HTTP ready on :%d", cfg.web_port);
    } else {
        ESP_LOGE(TAG, "HTTP server failed to start on :%d", cfg.web_port);
    }

    /* ── 16. MQTT bring-up ──────────────────────────────────── */
    awtrix_mqtt_init();

    /* ── 16b. Art-Net DMX receiver (Pack F) ───────────────────
     * Matches the original main.cpp boot sequence which always tried to
     * start the Art-Net listener; the receive task itself short-circuits
     * if CONFIG.artnetMode is false. Without this call, setting
     * artnetMode=true via /api/settings has no effect. */
    disp.startArtnet();

    /* ── 17. Show status on matrix ──────────────────────────── */
    s_matrix->clear();
    if (awtrix_wifi_is_connected()) {
        char info[48];
        snprintf(info, sizeof(info), "%s:%d", ipBuf, cfg.web_port);
        s_matrix->setCursor(0, 1);
        s_matrix->setTextColor(Matrix::Color(0, 255, 0));
        s_matrix->print(info);
    } else {
        s_matrix->setCursor(0, 1);
        s_matrix->setTextColor(Matrix::Color(255, 165, 0));
        s_matrix->print("AP:AWTRIX3");
    }
    s_matrix->show();
    ESP_LOGI(TAG, "AWTRIX3 %s ready – %s:%d", AWTRIX_VERSION, ipBuf, cfg.web_port);

    /* ── 18. Main loop (FreeRTOS rate-monotonic) ────────────── */
    TickType_t xLastWakeTime = xTaskGetTickCount();
    uint32_t   tickCount      = 0;     /* ~62.5 Hz at 16 ms */
    for (;;) {
        disp.tick();
        peri.tick();
        awtrix_mqtt_tick();

        /* ~5 Hz: UDP discovery responder. The original ran this every
         * frame which is wasteful — discovery clients only re-scan a few
         * times per second. */
        if ((tickCount % 12) == 0) {
            awtrix_udp_discovery_tick(cfg.hostname.c_str(), cfg.web_port);
        }

        /* ~1 Hz: New Year banner watcher (only fires within last 10 s of Dec 31). */
        if ((tickCount % 64) == 0) {
            disp.checkNewYear();
        }
        /* ~0.2 Hz: re-publish current app to MQTT for HA's currentApp sensor. */
        if ((tickCount % 320) == 0) {
            disp.sendAppLoop();
        }

        tickCount++;
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(16));
    }
}