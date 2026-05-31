#pragma once

#ifdef __cplusplus

#include "matrix_cpp.h"
#include <functional>

struct UiState;
class GifPlayer;

/* Pack D: menu overlay. When the awtrix_menumanager is in "active" state
 * (the user did a long-press on SELECT), this overlay paints a black
 * background + the current menu label/value over the entire matrix, so
 * the user can actually see what option they are tweaking. Mirrors the
 * MenuOverlay role from src/Overlays.cpp in the original firmware. */
using AwtrixMenuOverlayCallback = std::function<void(Matrix &, UiState &, GifPlayer *)>;

const AwtrixMenuOverlayCallback& awtrix_menu_overlay();

#endif /* __cplusplus */
