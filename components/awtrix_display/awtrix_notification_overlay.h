#pragma once

#ifdef __cplusplus

#include "matrix_cpp.h"
#include <functional>

struct UiState;
class GifPlayer;

using AwtrixNotificationOverlayCallback = std::function<void(Matrix &, UiState &, GifPlayer *)>;

const AwtrixNotificationOverlayCallback &awtrix_notification_overlay();

#endif /* __cplusplus */
