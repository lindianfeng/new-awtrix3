#include "awtrix_status_overlay.h"
#include "awtrix_color.h"   /* A4: AwtrixColors:: named constants */

extern "C" bool awtrix_wifi_is_ready(void) __attribute__((weak));
extern "C" bool awtrix_wifi_is_connected(void) __attribute__((weak));

const AwtrixStatusOverlayCallback &awtrix_status_overlay() {
    static AwtrixStatusOverlayCallback overlay = [](Matrix &m, UiState &, GifPlayer *) {
        if (&awtrix_wifi_is_ready && !awtrix_wifi_is_ready()) return;
        bool sta = &awtrix_wifi_is_connected ? awtrix_wifi_is_connected() : false;
        uint32_t col = sta ? AwtrixColors::kStatusOnline
                           : AwtrixColors::kStatusWiFiDown;
        m.drawPixel(m.width() - 1, 0, col);
    };
    return overlay;
}
