#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* TCP game-controller socket (port matches the original AWTRIX3 8080).
 * Spawns a dedicated FreeRTOS task that accepts one client at a time and
 * forwards newline-terminated lines to awtrix_game_controller_input(). */
void awtrix_tcp_game_start(uint16_t port);
void awtrix_tcp_game_stop(void);

/* Push a string to the currently-connected client (no-op when none). */
void awtrix_tcp_game_send(const char* line);

#ifdef __cplusplus
}
#endif
