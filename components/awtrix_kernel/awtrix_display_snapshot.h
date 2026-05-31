#pragma once

#ifdef __cplusplus

#include "awtrix_hal.h"
#include <stdint.h>
#include <string>

class AwtrixDisplaySnapshot {
public:
    static AwtrixDisplaySnapshot &get();

    void updateFrameRgb888(const uint32_t *pixels, int count, int width, int height);
    std::string screenJson() const;

private:
    AwtrixDisplaySnapshot() = default;

    mutable portMUX_TYPE m_lock = portMUX_INITIALIZER_UNLOCKED;
    uint32_t m_pixels[MATRIX_WIDTH * MATRIX_HEIGHT] = {0};
    int m_width = MATRIX_WIDTH;
    int m_height = MATRIX_HEIGHT;
    bool m_valid = false;
};

#define DISPLAY_SNAPSHOT (AwtrixDisplaySnapshot::get())

#endif /* __cplusplus */
