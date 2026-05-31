#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ── Dictionary (port of src/Dictionary.{h,cpp}) ──────────────────
 * Provides localized short names for weekdays and months. Original
 * shipped 25 locales; this port ships 7 common ones (en/de/fr/es/it/zh/ja);
 * unknown locales fall back to English.
 *
 * Indexing:
 *   - dayShort(lang, idx)  : idx 0..6  (Mon..Sun)
 *   - monthShort(lang, idx): idx 0..11 (Jan..Dec)
 */

const char* dict_day_short(const char* lang, int day_idx); /* 3-letter */
const char* dict_month_short(const char* lang, int month_idx); /* 3-letter */
const char* dict_day_long(const char* lang, int day_idx);

#ifdef __cplusplus
}
#endif
