# awtrix_display — unified display subsystem.
#
# Bundles three previously-separate components (display device, notifications,
# render pipeline) that all operate on the same Matrix surface and formed a
# strict one-way dependency chain. Round 5 (dead-code cleanup) further
# removed three abandoned abstractions: Canvas, DisplayDevice /
# MatrixDisplayDevice, and Framebuffer — all had zero call sites in the
# real render path (which goes Matrix→CRGB→led_matrix HAL→RMT directly).
#
# Round 5 also moved effects_core from awtrix_core into here: all fx_* and
# EffectOverlay primitives draw pixels directly on Matrix, so they belong to
# the display layer, not to the app/business orchestration layer. This
# eliminates the awkward __attribute__((weak)) reverse-link that
# awtrix_notification_overlay.cpp used to need to reach back into core.
#
# Layers from low to high:
#   - Device           : led_matrix (driver), matrix_cpp (Matrix C++ wrapper),
#                        awtrix_font, GifPlayer
#   - Effects          : 21 fx_* shader-style routines + EffectOverlay
#                        (weather), exposed via effects_core.h
#   - Notifications    : AwtrixNotificationManager queue + overlay callback
#   - Render pipeline  : AwtrixRenderEngine + status overlay + overlay registry
#
# Public REQUIRES:
#   - awtrix_hal       : CRGB, MATRIX_WIDTH/HEIGHT, TAG_* — surfaces in
#                        matrix_cpp.h.
#   - awtrix_kernel    : AwtrixEventBus (EVENTS.* in notification_overlay.cpp)
#                        + cross-cutting contracts that flow through the
#                        notification / render pipeline.
#   - awtrix_config    : CONFIG.* fields read by notification overlay and the
#                        render engine when deciding visibility / styling.
#
# PRIV_REQUIRES:
#   - led_strip / esp_timer : driver + monotonic clock used by the device layer.
#   - cjson                 : effects_core JSON settings parser.
idf_component_register(
        SRCS "led_matrix.c"
             "matrix_cpp.cpp"
             "awtrix_font.cpp"
             "GifPlayer.cpp"
             "effects_core.cpp"
             "awtrix_notifications.cpp"
             "awtrix_notification_overlay.cpp"
             "awtrix_render.cpp"
             "awtrix_status_overlay.cpp"
             "awtrix_menu_overlay.cpp"
             "awtrix_overlay_registry.cpp"
             "awtrix_icon_loader.cpp"
        INCLUDE_DIRS "."
        REQUIRES awtrix_hal awtrix_kernel awtrix_config
        PRIV_REQUIRES led_strip esp_timer cjson spiffs espressif__esp_new_jpeg
)
