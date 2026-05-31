#include "awtrix_overlay_registry.h"

std::vector<AwtrixOverlayCallback> awtrix_default_overlays() {
    /* Pack D: menu overlay sits at the top — when active it fills the
     * whole matrix with its label, hiding any app/notification underneath
     * (mirrors the role of MenuOverlay in src/Overlays.cpp). The status
     * pixel stays last so the WiFi/MQTT indicator is always visible. */
    return {
        awtrix_menu_overlay(),
        awtrix_notification_overlay(),
        awtrix_status_overlay(),
    };
}
