#pragma once

#include "boards/ulanzi_s3.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Core types ──────────────────────────────────────────────── */
typedef struct
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

/* ── Startup helpers ─────────────────────────────────────────── */
void awtrix_init(void);
void awtrix_start_tasks(void);

#ifdef __cplusplus
}
#endif
