#pragma once
#include "awtrix_hal.h"
#include "awtrix_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Settings initialization ─────────────────────────────────── */
void awtrix_settings_init(void);

/* ── Filesystem init ─────────────────────────────────────────── */
bool awtrix_fs_mount(void);
bool awtrix_fs_file_exists(const char* path);

#ifdef __cplusplus
}
#endif