#pragma once

#ifdef __cplusplus

#include "awtrix_command.h"
#include <stddef.h>

void awtrix_command_bus_init(void);
bool awtrix_command_bus_post(const AwtrixCommand& command, uint32_t timeout_ms = 0);
bool awtrix_command_bus_receive(AwtrixCommand& out, uint32_t timeout_ms = 0);
size_t awtrix_command_bus_depth(void);

#endif /* __cplusplus */
