#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Icon database (port of src/icons.h, AWTRIX 2 icon set) ─────
 * Each icon is an 8x8 RGB565 bitmap (64 uint16_t).
 * Lookup by numeric id (same scheme as the original LaMetric/Awtrix
 * "icon id"). icon_get returns NULL if id is unknown.
 */
typedef struct {
    uint16_t id;
    const uint16_t *data; /* 64 RGB565 pixels */
} awtrix_icon_t;

const uint16_t *icon_get(uint16_t id);
int icon_count(void);

/* Iterate over the entire icon table — used by stats/inventory APIs. */
const awtrix_icon_t *icon_at(int idx);

#ifdef __cplusplus
}
#endif
