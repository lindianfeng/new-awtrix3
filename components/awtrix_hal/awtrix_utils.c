#include "awtrix_utils.h"

#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "utils";
static nvs_handle_t s_handle;
static bool s_inited = false;

/* ── ring buffer ─────────────────────────────────────────────── */
void awtrix_rb_init(ring_buf_t* rb, uint8_t* storage, int size)
{
    rb->buf = storage;
    rb->size = size;
    rb->head = 0;
    rb->tail = 0;
}

int awtrix_rb_push(ring_buf_t* rb, uint8_t b)
{
    int next = (rb->head + 1) % rb->size;
    if (next == rb->tail) return -1; /* full */
    rb->buf[rb->head] = b;
    rb->head = next;
    return 0;
}

int awtrix_rb_pop(ring_buf_t* rb)
{
    if (rb->head == rb->tail) return -1; /* empty */
    uint8_t b = rb->buf[rb->tail];
    rb->tail = (rb->tail + 1) % rb->size;
    return b;
}

int awtrix_rb_avail(const ring_buf_t* rb)
{
    if (rb->head >= rb->tail)
        return rb->head - rb->tail;
    return rb->size - rb->tail + rb->head;
}

/* ── NVS settings ────────────────────────────────────────────── */
void awtrix_nvs_init(void)
{
    if (s_inited) return;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }
    err = nvs_open("awtrix", NVS_READWRITE, &s_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }
    s_inited = true;
}

#define NVS_CHECK(b, key) { esp_err_t e = (b); if (e != ESP_OK) ESP_LOGW(TAG, "NVS err %s on key %s", esp_err_to_name(e), key); }

void awtrix_settings_save_bool(const char* key, bool val)
{
    NVS_CHECK(nvs_set_u8(s_handle, key, val ? 1 : 0), key);
    nvs_commit(s_handle);
}

bool awtrix_settings_load_bool(const char* key, bool def)
{
    uint8_t v;
    if (nvs_get_u8(s_handle, key, &v) == ESP_OK) return v != 0;
    return def;
}

void awtrix_settings_save_u8(const char* key, uint8_t val)
{
    NVS_CHECK(nvs_set_u8(s_handle, key, val), key);
    nvs_commit(s_handle);
}

uint8_t awtrix_settings_load_u8(const char* key, uint8_t def)
{
    uint8_t v;
    return nvs_get_u8(s_handle, key, &v) == ESP_OK ? v : def;
}

void awtrix_settings_save_u16(const char* key, uint16_t val)
{
    NVS_CHECK(nvs_set_u16(s_handle, key, val), key);
    nvs_commit(s_handle);
}

uint16_t awtrix_settings_load_u16(const char* key, uint16_t def)
{
    uint16_t v;
    return nvs_get_u16(s_handle, key, &v) == ESP_OK ? v : def;
}

void awtrix_settings_save_u32(const char* key, uint32_t val)
{
    NVS_CHECK(nvs_set_u32(s_handle, key, val), key);
    nvs_commit(s_handle);
}

uint32_t awtrix_settings_load_u32(const char* key, uint32_t def)
{
    uint32_t v;
    return nvs_get_u32(s_handle, key, &v) == ESP_OK ? v : def;
}

void awtrix_settings_save_float(const char* key, float val)
{
    NVS_CHECK(nvs_set_blob(s_handle, key, &val, sizeof(val)), key);
    nvs_commit(s_handle);
}

float awtrix_settings_load_float(const char* key, float def)
{
    float v;
    size_t s = sizeof(v);
    return nvs_get_blob(s_handle, key, &v, &s) == ESP_OK ? v : def;
}

void awtrix_settings_save_str(const char* key, const char* val)
{
    if (val) NVS_CHECK(nvs_set_str(s_handle, key, val), key);
    nvs_commit(s_handle);
}

void awtrix_settings_load_str(const char* key, char* buf, size_t buflen, const char* def)
{
    size_t len = buflen;
    if (nvs_get_str(s_handle, key, buf, &len) != ESP_OK && def)
    {
        strncpy(buf, def, buflen);
    }
}

void awtrix_settings_save_blob(const char* key, const void* data, size_t len)
{
    if (data) NVS_CHECK(nvs_set_blob(s_handle, key, data, len), key);
    nvs_commit(s_handle);
}

bool awtrix_settings_load_blob(const char* key, void* buf, size_t* len)
{
    esp_err_t e = nvs_get_blob(s_handle, key, buf, len);
    return e == ESP_OK;
}

void awtrix_settings_erase_all(void)
{
    nvs_erase_all(s_handle);
    nvs_commit(s_handle);
}