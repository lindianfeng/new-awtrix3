#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ── MenuManager (port of src/MenuManager.cpp) ────────────────────
 * Long-press SELECT enters/exits the configuration menu. While the menu
 * is active, LEFT/RIGHT cycle through entries and short SELECT applies.
 *
 * DisplayManager forwards selectButton/selectButtonLong here through
 * the weak symbols `awtrix_menu_select_short` and `awtrix_menu_select_long`.
 */

void awtrix_menu_enter(void);
void awtrix_menu_exit(void);
bool awtrix_menu_active(void);

/* Button hooks (also exposed under the weak symbols expected by DM). */
void awtrix_menu_left(void);
void awtrix_menu_right(void);
void awtrix_menu_select_short(void);
void awtrix_menu_select_long(void);

/* Returns the label for the currently selected menu entry (read-only). */
const char *awtrix_menu_label(void);

#ifdef __cplusplus
}
#endif
