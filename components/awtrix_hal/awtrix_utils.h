#pragma once
#include "awtrix_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── General-purpose ring buffer ─────────────────────────────── */
typedef struct {
    uint8_t *buf;
    int      head;
    int      tail;
    int      size;
} ring_buf_t;

void awtrix_rb_init(ring_buf_t *rb, uint8_t *storage, int size);
int  awtrix_rb_push(ring_buf_t *rb, uint8_t b);
int  awtrix_rb_pop(ring_buf_t *rb);
int  awtrix_rb_avail(const ring_buf_t *rb);

/* ── Simple NVS-based key-value persistence (replaces Arduino Preferences) ── */
void    awtrix_nvs_init(void);
void    awtrix_settings_save_bool(const char *key, bool val);
bool    awtrix_settings_load_bool(const char *key, bool def);
void    awtrix_settings_save_u8(const char *key, uint8_t val);
uint8_t awtrix_settings_load_u8(const char *key, uint8_t def);
void    awtrix_settings_save_u16(const char *key, uint16_t val);
uint16_t awtrix_settings_load_u16(const char *key, uint16_t def);
void    awtrix_settings_save_u32(const char *key, uint32_t val);
uint32_t awtrix_settings_load_u32(const char *key, uint32_t def);
void    awtrix_settings_save_str(const char *key, const char *val);
void    awtrix_settings_load_str(const char *key, char *buf, size_t buflen, const char *def);
void    awtrix_settings_save_float(const char *key, float val);
float   awtrix_settings_load_float(const char *key, float def);
void    awtrix_settings_save_blob(const char *key, const void *data, size_t len);
bool    awtrix_settings_load_blob(const char *key, void *buf, size_t *len);
void    awtrix_settings_erase_all(void);

/* ── C++ convenience macros ───────────────────────────────────── */
#ifdef __cplusplus
#define SETTINGS_BEGIN()    awtrix_nvs_init()
#define SETTINGS_BEGIN_P()  /* already inited, no-op */
#define SETTINGS_GET_U8(k,d)  awtrix_settings_load_u8(k,d)
#define SETTINGS_GET_U16(k,d) awtrix_settings_load_u16(k,d)
#define SETTINGS_GET_U32(k,d) awtrix_settings_load_u32(k,d)
#define SETTINGS_GET_BOOL(k,d) awtrix_settings_load_bool(k,d)
#endif

#ifdef __cplusplus
}
#endif