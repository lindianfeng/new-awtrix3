#include "awtrix_menu_overlay.h"
#include "awtrix_globals.h"
#include <cstring>

/* Pack D: weakly link the menu-manager C ABI so this display-layer file
 * doesn't pick up a hard dep on awtrix_core (which would create a cycle).
 * When the menu manager is missing (e.g. unit tests) both symbols resolve
 * to nullptr and the overlay becomes a silent no-op. */
extern "C" bool awtrix_menu_active(void) __attribute__
((weak));
extern "C" const char* awtrix_menu_label(void) __attribute__
((weak));

const AwtrixMenuOverlayCallback& awtrix_menu_overlay()
{
    static AwtrixMenuOverlayCallback overlay = [](Matrix& m, UiState&, GifPlayer*)
    {
        if (!(&awtrix_menu_active) || !awtrix_menu_active()) return;
        const char* label = (&awtrix_menu_label) ? awtrix_menu_label() : "";
        if (!label || !*label) return;

        /* Black backdrop so the underlying app doesn't bleed through. */
        m.fillRect(0, 0, m.width(), m.height(), 0x000000);

        /* Centered label, using the configured text color. The matrix
         * font in this port draws glyphs as 4 px wide each, same as the
         * notification renderer. */
        int len = (int)strlen(label);
        int textW = len * 4;
        int x = (m.width() - textW) / 2;
        if (x < 0)
        {
            /* Long labels: clip to the left edge. The original used a
             * scrolling marquee, but most menu strings fit in 32 px. */
            x = 0;
        }
        m.setTextColor(CONFIG.textColor888);
        m.setCursor(x, 1);
        m.print(label);
    };
    return overlay;
}
