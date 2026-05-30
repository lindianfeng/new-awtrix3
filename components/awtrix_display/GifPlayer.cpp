#include "GifPlayer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstdio>
#include <cstdlib>

#define TAG "gif"

bool GifPlayer::load(const std::string &path) {
    if (m_data) { free(m_data); m_data = nullptr; }
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) { fclose(fp); return false; }
    m_data = (uint8_t *)malloc(sz);
    if (!m_data) { fclose(fp); return false; }
    size_t n = fread(m_data, 1, sz, fp);
    fclose(fp);
    if ((int)n != sz) { free(m_data); m_data = nullptr; return false; }
    /* Default geometry: 8x8 RGB565 frames stacked back-to-back. */
    m_w = 8; m_h = 8;
    int bytesPerFrame = m_w * m_h * 2;
    m_frames = (int)(sz / bytesPerFrame);
    m_cur = 0; m_lastTick = 0;
    if (m_frames <= 0) { free(m_data); m_data = nullptr; return false; }
    ESP_LOGI(TAG, "Loaded %s : %d frames %dx%d", path.c_str(), m_frames, m_w, m_h);
    return true;
}

void GifPlayer::draw(Matrix &m, int16_t x, int16_t y) {
    if (!m_data || m_frames == 0) return;
    uint64_t now = esp_timer_get_time() / 1000;
    if (m_lastTick == 0) m_lastTick = now;
    if (now - m_lastTick >= 80) {                /* ~12 fps */
        m_lastTick = now;
        m_cur = (m_cur + 1) % m_frames;
    }
    const uint16_t *frame = (const uint16_t *)(m_data + m_cur * m_w * m_h * 2);
    m.drawRGBBitmap(x, y, frame, m_w, m_h);
}

void GifPlayer::reset() {
    m_cur = 0; m_lastTick = 0;
}
