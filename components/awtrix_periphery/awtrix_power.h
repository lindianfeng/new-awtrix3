#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ── Power / sleep manager (port of src/PowerManager.cpp) ────── */

/* Enter ESP32-S3 deep sleep for `seconds` (0 = parser default = 60s).
 * The original Awtrix3 boots from EXT0 wakeup on the SELECT button or
 * a timer. We wire SELECT (GPIO 14) as EXT1 wake source. */
void awtrix_power_sleep(unsigned long seconds);

/* Accepts {"seconds":N} or {"hours":H,"minutes":M,"seconds":S}. */
void awtrix_power_sleep_parser(const char *json);

#ifdef __cplusplus
}
#endif
