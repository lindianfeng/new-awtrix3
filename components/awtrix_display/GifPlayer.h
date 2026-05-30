#pragma once

#ifdef __cplusplus

#include <stdint.h>
#include <string>
#include "matrix_cpp.h"

/* ── GIF / animation player (port of src/GifPlayer.h) ─────────────
 * Original used TJpg_Decoder for JPEG + custom GIF decoder. ESP-IDF v6
 * doesn't ship a built-in GIF decoder; this port supports two formats
 * commonly produced by the AWTRIX icon converter and used by the app:
 *
 *   1. Raw 8x8 RGB565 ("bitmap"). 128 bytes per frame, N frames back-to-back.
 *      File extension expected: ".bmp" or ".rgb".
 *   2. Static 8x8 PNG fallback: not implemented (returns false).
 *
 * Frame timing uses a fixed CONFIG-defined delay (~80ms) — matching the
 * default GifPlayer animation cadence in the original. */

class GifPlayer
{
public:
    GifPlayer() = default;

    /* Load an animation file from /spiffs. Returns true on success. */
    bool load(const std::string& path);
    /* Draw current frame at (x,y) onto matrix; advances frame automatically. */
    void draw(Matrix& m, int16_t x, int16_t y);
    /* Reset internal state (called when switching apps). */
    void reset();

    int width() const { return m_w; }
    int height() const { return m_h; }
    int frames() const { return m_frames; }

private:
    uint8_t* m_data = nullptr; /* raw frame data (frames × w*h*2 bytes RGB565) */
    int m_w = 0, m_h = 0;
    int m_frames = 0;
    int m_cur = 0;
    uint64_t m_lastTick = 0;
};

#endif /* __cplusplus */
