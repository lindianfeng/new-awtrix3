#include "awtrix_storage.h"

#include <sys/stat.h>

#include "esp_spiffs.h"
#include "esp_log.h"

static const char *TAG = TAG_STORE;

void awtrix_settings_init(void) {
    awtrix_nvs_init();
    ESP_LOGI(TAG, "Settings initialized");
}

bool awtrix_fs_mount(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "fs",
        .max_files = 8,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "SPIFFS mounted at /spiffs");
    return true;
}

bool awtrix_fs_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}