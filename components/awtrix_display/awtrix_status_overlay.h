#pragma once

#ifdef __cplusplus

#include "matrix_cpp.h"
#include <functional>

struct UiState;
class GifPlayer;

using AwtrixStatusOverlayCallback = std::function<void(Matrix &, UiState &, GifPlayer *)>;

const AwtrixStatusOverlayCallback& awtrix_status_overlay();

#endif /* __cplusplus */
