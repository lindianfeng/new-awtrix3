#include "awtrix_display_snapshot.h"

#include <cJSON.h>

AwtrixDisplaySnapshot &AwtrixDisplaySnapshot::get() {
    static AwtrixDisplaySnapshot instance;
    return instance;
}

void AwtrixDisplaySnapshot::updateFrameRgb888(const uint32_t *pixels, int count, int width, int height) {
    if (!pixels || count <= 0) return;
    const int maxPixels = MATRIX_WIDTH * MATRIX_HEIGHT;
    if (count > maxPixels) count = maxPixels;

    portENTER_CRITICAL(&m_lock);
    for (int i = 0; i < count; ++i) m_pixels[i] = pixels[i] & 0xFFFFFF;
    for (int i = count; i < maxPixels; ++i) m_pixels[i] = 0;
    m_width = width > 0 ? width : MATRIX_WIDTH;
    m_height = height > 0 ? height : MATRIX_HEIGHT;
    m_valid = true;
    portEXIT_CRITICAL(&m_lock);
}

std::string AwtrixDisplaySnapshot::screenJson() const {
    uint32_t copy[MATRIX_WIDTH * MATRIX_HEIGHT];
    int width = MATRIX_WIDTH;
    int height = MATRIX_HEIGHT;
    bool valid = false;

    portENTER_CRITICAL(&m_lock);
    for (int i = 0; i < MATRIX_WIDTH * MATRIX_HEIGHT; ++i) copy[i] = m_pixels[i];
    width = m_width;
    height = m_height;
    valid = m_valid;
    portEXIT_CRITICAL(&m_lock);

    if (!valid) return "[]";
    int total = width * height;
    const int maxPixels = MATRIX_WIDTH * MATRIX_HEIGHT;
    if (total < 0 || total > maxPixels) total = maxPixels;

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return "[]";
    for (int i = 0; i < total; ++i) cJSON_AddItemToArray(arr, cJSON_CreateNumber((int)copy[i]));
    char *s = cJSON_PrintUnformatted(arr);
    std::string out(s ? s : "[]");
    if (s) cJSON_free(s);
    cJSON_Delete(arr);
    return out;
}
