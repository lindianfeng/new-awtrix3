#pragma once

#ifdef __cplusplus

#include "awtrix_http.h"

/* ── Registers all AWTRIX3 REST API endpoints ────────────────────
 * Ported from the original ServerManager.cpp addHandler() function.
 */
void awtrix_api_register_routes(AwtrixHttpServer & srv);

#endif /* __cplusplus */