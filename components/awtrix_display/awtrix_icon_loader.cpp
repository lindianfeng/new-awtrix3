/**
 * Pack K + L: SPIFFS-backed icon loader.
 *
 * Honours the .rgb565 / .jpg lookup order documented in the header. The
 * 8x8 icon size is hard-coded to match the LaMetric icon convention used
 * by every existing AWTRIX customApp / icon bank.
 */
#include "awtrix_icon_loader.h"
#include "awtrix_hal.h"   /* TAG_DISPLAY */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_jpeg_dec.h"

#define TAG TAG_DISPLAY

#define ICON_W 8
#define ICON_H 8
#define ICON_PIXELS (ICON_W * ICON_H)

/* Convert an RGB888 pixel to RGB565 (high byte first). */
static inline uint16_t rgb888_to_565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Try to read 128 bytes of raw RGB565 from `path` into `out`.
 * On success returns true. The on-disk byte order is little-endian
 * uint16_t (lo byte first), matching the format produced by the
 * awtrix-companion python tool. */
static bool load_raw_rgb565(const char* path, uint16_t out[ICON_PIXELS])
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (st.st_size < (off_t)(ICON_PIXELS * 2)) return false;
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    size_t n = fread(out, 1, ICON_PIXELS * 2, fp);
    fclose(fp);
    return n == (size_t)(ICON_PIXELS * 2);
}

/* Decode `path` as JPEG, scale-decode the smallest available size, then
 * sub-sample to 8x8. esp_new_jpeg lets the decoder pick a 1/2/4/8 scale
 * factor — for typical 32x32 LaMetric icons that means we get 4x4..32x32
 * raw blocks back depending on JPEG header. We always finish with a manual
 * 8x8 box filter so the cosumer just sees an icon-sized RGB565 buffer. */
static bool load_jpeg(const char* path, uint16_t out[ICON_PIXELS])
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (st.st_size <= 0 || st.st_size > 32 * 1024)
    {
        /* Cap input at 32 KB to keep RAM usage bounded; real LaMetric icons
         * are usually well under 4 KB. */
        ESP_LOGW(TAG, "icon JPEG too large: %s (%lld B)", path, (long long)st.st_size);
        return false;
    }
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    uint8_t* buf = (uint8_t*)malloc((size_t)st.st_size);
    if (!buf)
    {
        fclose(fp);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if (got != (size_t)st.st_size)
    {
        free(buf);
        return false;
    }

    jpeg_dec_config_t cfg = {};
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB888;
    cfg.scale = {0, 0};
    cfg.clipper = {0, 0};
    cfg.rotate = JPEG_ROTATE_0D;
    cfg.block_enable = false;

    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK || !dec)
    {
        free(buf);
        return false;
    }

    jpeg_dec_io_t io = {};
    jpeg_dec_header_info_t hdr = {};
    io.inbuf = buf;
    io.inbuf_len = (int)got;

    if (jpeg_dec_parse_header(dec, &io, &hdr) != JPEG_ERR_OK)
    {
        jpeg_dec_close(dec);
        free(buf);
        return false;
    }

    int outsize = 0;
    if (jpeg_dec_get_outbuf_len(dec, &outsize) != JPEG_ERR_OK || outsize <= 0)
    {
        jpeg_dec_close(dec);
        free(buf);
        return false;
    }
    uint8_t* rgb = (uint8_t*)malloc((size_t)outsize);
    if (!rgb)
    {
        jpeg_dec_close(dec);
        free(buf);
        return false;
    }

    io.outbuf = rgb;
    jpeg_error_t err = jpeg_dec_process(dec, &io);
    jpeg_dec_close(dec);
    free(buf);
    if (err != JPEG_ERR_OK)
    {
        free(rgb);
        return false;
    }

    /* hdr.width / hdr.height tell us the decoded RGB888 dimensions. Box-filter
     * down to 8x8 by averaging each source block. */
    int W = hdr.width;
    int H = hdr.height;
    if (W <= 0 || H <= 0)
    {
        free(rgb);
        return false;
    }

    for (int oy = 0; oy < ICON_H; oy++)
    {
        for (int ox = 0; ox < ICON_W; ox++)
        {
            int x0 = (ox * W) / ICON_W;
            int x1 = ((ox + 1) * W) / ICON_W;
            int y0 = (oy * H) / ICON_H;
            int y1 = ((oy + 1) * H) / ICON_H;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
            {
                for (int xx = x0; xx < x1; xx++)
                {
                    const uint8_t* p = &rgb[(yy * W + xx) * 3];
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    n++;
                }
            }
            if (!n) n = 1;
            out[oy * ICON_W + ox] = rgb888_to_565((uint8_t)(r / n),
                                                  (uint8_t)(g / n),
                                                  (uint8_t)(b / n));
        }
    }
    free(rgb);
    return true;
}

bool awtrix_icon_load_rgb565(const char* name, uint16_t out_rgb565[64])
{
    if (!name || !*name || !out_rgb565) return false;
    char path[96];

    /* If the caller passed an extension, try that exact path first. */
    const char* dot = strrchr(name, '.');
    if (dot)
    {
        snprintf(path, sizeof(path), "/spiffs/ICONS/%s", name);
        if (strcasecmp(dot, ".rgb565") == 0) return load_raw_rgb565(path, out_rgb565);
        if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)
            return load_jpeg(path, out_rgb565);
        /* Unknown ext: try raw first, then JPEG as a last resort. */
        if (load_raw_rgb565(path, out_rgb565)) return true;
        return load_jpeg(path, out_rgb565);
    }

    /* No extension: try the .rgb565 fast path, then .jpg. */
    snprintf(path, sizeof(path), "/spiffs/ICONS/%s.rgb565", name);
    if (load_raw_rgb565(path, out_rgb565)) return true;

    snprintf(path, sizeof(path), "/spiffs/ICONS/%s.jpg", name);
    if (load_jpeg(path, out_rgb565)) return true;

    snprintf(path, sizeof(path), "/spiffs/ICONS/%s.jpeg", name);
    return load_jpeg(path, out_rgb565);
}
