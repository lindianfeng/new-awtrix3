#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ── Tiny games (port of src/Games/AwtrixSays + SlotMachine + GameManager)
 *
 * These are autonomous mini-games that take over the display when invoked.
 * They are triggered via /api endpoints and exit back to the main app
 * cycle when finished. While a game is active, DisplayManager pauses its
 * normal tick by setting CONFIG.gameActive = true.
 */

void awtrix_game_awtrix_says_start(void);
void awtrix_game_slot_machine_start(void);
void awtrix_game_tick(void);                  /* call every frame */
bool awtrix_game_active(void);

/* Controller input: a single line received from the TCP:8080 socket
 * (port matches the original AWTRIX3 ServerManager.cpp). The string is
 * routed to whichever game is currently active. */
void awtrix_game_controller_input(const char *line);

#ifdef __cplusplus
}
#endif
