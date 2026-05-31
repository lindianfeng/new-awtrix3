#pragma once

#ifdef __cplusplus

#include "awtrix_status_overlay.h"
#include "awtrix_notification_overlay.h"
#include "awtrix_menu_overlay.h"
#include <vector>

using AwtrixOverlayCallback = AwtrixStatusOverlayCallback;

std::vector<AwtrixOverlayCallback> awtrix_default_overlays();

#endif /* __cplusplus */
