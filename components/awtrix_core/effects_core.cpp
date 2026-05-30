#include "effects_core.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cJSON.h>
#include "esp_timer.h"

#define TAG "fx"

static uint64_t _now() { return esp_timer_get_time() / 1000; }
[[maybe_unused]] static int _rand_range(int lo, int hi) { return lo + (std::rand() % (hi - lo + 1)); }

/* ── HSV→RGB helper ─────────────────────────────────────────── */
static CRGB hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v) {
    if (s == 0) return CRGB(v, v, v);
    uint8_t region = h / 43;
    uint8_t rem = (h - region * 43) * 6;
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
    switch (region) {
        case 0: return CRGB(v, t, p);
        case 1: return CRGB(q, v, p);
        case 2: return CRGB(p, v, t);
        case 3: return CRGB(p, q, v);
        case 4: return CRGB(t, p, v);
        default:return CRGB(v, p, q);
    }
}

[[maybe_unused]] static CRGB lerpCRGB(const CRGB &a, const CRGB &b, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return CRGB((uint8_t)(a.r + (b.r - a.r) * t),
                (uint8_t)(a.g + (b.g - a.g) * t),
                (uint8_t)(a.b + (b.b - a.b) * t));
}

static void fadeAllBy(CRGB grid[32][8], uint8_t amount) {
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++) {
            grid[i][j].r = (grid[i][j].r * (255 - amount)) >> 8;
            grid[i][j].g = (grid[i][j].g * (255 - amount)) >> 8;
            grid[i][j].b = (grid[i][j].b * (255 - amount)) >> 8;
        }
}

static void blitGrid(Matrix &m, int16_t x, int16_t y, CRGB grid[32][8]) {
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++)
            m.drawPixel(x + i, y + j, grid[i][j]);
}

/* ── Pacifica ────────────────────────────────────────────────── */
void fx_Pacifica(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static uint32_t time = 0;
    time += (uint32_t)s->speed;
    for (int i = 0; i < 32; i++) {
        for (int j = 0; j < 8; j++) {
            int ulx = (time / 8) - (i * 16);
            int uly = (time / 4) + (j * 16);
            int v = 0;
            v += (sin(ulx * 6 * M_PI / 32768.0 + time / 2.0 * M_PI / 32768.0) + 1.0) * 127.0 + 127;
            v += (sin(uly * 9 * M_PI / 32768.0 + time / 2.0 * M_PI / 32768.0) + 1.0) * 127.0 + 127;
            v += (sin((ulx * 7 + uly * 2) * M_PI / 32768.0 - time * M_PI / 32768.0) + 1.0) * 127.0;
            v /= 3;
            m.drawPixel(x + i, y + j, hsv_to_rgb(v, 255, 200));
        }
    }
}

/* ── Plasma ──────────────────────────────────────────────────── */
void fx_Plasma(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static float time = 0;
    time += 0.02f * (float)s->speed;
    for (int i = 0; i < 32; i++) {
        for (int j = 0; j < 8; j++) {
            float v1 = sinf(i * 0.5f + time);
            float v2 = sinf(j * 0.5f + time * 0.7f);
            float v3 = sinf((i + j) * 0.5f + time * 0.5f);
            uint8_t val = (uint8_t)(((v1 + v2 + v3) / 3.0f + 1.0f) * 127.5f);
            m.drawPixel(x + i, y + j, hsv_to_rgb(val, 255, 200));
        }
    }
}

/* ── Rainbow ─────────────────────────────────────────────────── */
void fx_Rainbow(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static uint32_t phase = 0;
    phase += (uint32_t)s->speed;
    for (int i = 0; i < 32; i++) {
        for (int j = 0; j < 8; j++) {
            uint8_t h = (uint8_t)((i * 8 + j * 4 + phase) & 0xFF);
            m.drawPixel(x + i, y + j, hsv_to_rgb(h, 255, 200));
        }
    }
}

/* ── TheaterChase ────────────────────────────────────────────── */
void fx_TheaterChase(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static uint32_t last = 0;
    static int j = 0;
    if (_now() - last > (uint32_t)(100 - s->speed * 10)) { last = _now(); j++; }
    for (int i = 0; i < 32; i++) {
        for (int k = 0; k < 8; k++) {
            if ((i + j) % 3 == 0) {
                m.drawPixel(x + i, y + k, hsv_to_rgb((uint8_t)(i * 8), 255, 200));
            } else {
                m.drawPixel(x + i, y + k, CRGB(0, 0, 0));
            }
        }
    }
}

/* ── Sparkles ────────────────────────────────────────────────── */
void fx_Sparkles(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static uint32_t last = 0;
    if (_now() - last < 50) return;
    last = _now();
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++)
            m.drawPixel(x + i, y + j, CRGB(0, 0, 0));
    for (int k = 0; k < (int)(s->speed * 3); k++) {
        int rx = std::rand() % 32, ry = std::rand() % 8;
        uint8_t v = 50 + (std::rand() % 205);
        m.drawPixel(x + rx, y + ry, CRGB(v, v, v));
    }
}

/* ── Noise ───────────────────────────────────────────────────── */
void fx_Noise(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static uint32_t phase = 0;
    phase += (uint32_t)s->speed;
    for (int i = 0; i < 32; i++) {
        for (int j = 0; j < 8; j++) {
            int val = (i * 3 + j * 7 + phase) % 256;
            m.drawPixel(x + i, y + j, CRGB(val, val, val));
        }
    }
}

/* ── Rain ────────────────────────────────────────────────────── */
void fx_Rain(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static int drops[32] = {0};
    static uint32_t last = 0;
    if (_now() - last > (uint32_t)(200 - s->speed * 15)) {
        last = _now();
        for (int i = 0; i < 32; i++) {
            drops[i] += (std::rand() % 3) ? 0 : 1;
            if (drops[i] >= 8) drops[i] = 0;
        }
    }
    for (int i = 0; i < 32; i++) {
        for (int j = 0; j < 8; j++) {
            if (j == drops[i]) m.drawPixel(x + i, y + j, CRGB(0, 200, 255));
            else              m.drawPixel(x + i, y + j, CRGB(0, 0, 0));
        }
    }
}

/* ── Matrix (digital-rain) ──────────────────────────────────── */
void fx_Matrix(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static CRGB grid[32][8] = {};
    static uint32_t lastMove = 0;
    const CRGB spawn(175, 255, 175);
    const CRGB trail(27, 130, 39);
    uint32_t period = (uint32_t)(180 - s->speed * 15);
    if (_now() - lastMove >= period) {
        lastMove = _now();
        for (int i = 0; i < 32; i++)
            for (int j = 7; j > 0; j--) grid[i][j] = grid[i][j - 1];
        for (int i = 0; i < 32; i++) {
            if (grid[i][0].r == spawn.r && grid[i][0].g == spawn.g && grid[i][0].b == spawn.b)
                grid[i][0] = trail;
            else { grid[i][0].r >>= 1; grid[i][0].g >>= 1; grid[i][0].b >>= 1; }
            if ((std::rand() & 0xFF) < 8) grid[i][0] = spawn;
        }
    }
    blitGrid(m, x, y, grid);
}

/* ── SwirlIn ─────────────────────────────────────────────────── */
void fx_SwirlIn(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static uint32_t last = 0;
    static uint16_t angle = 0;
    if (_now() - last > (uint32_t)(100 - s->speed * 10)) { last = _now(); angle += 4; }
    float cx = 16, cy = 4;
    float maxd = sqrtf(cx * cx + cy * cy);
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++) {
            float dx = cx - i, dy = cy - j;
            float d = sqrtf(dx * dx + dy * dy);
            uint8_t hue = (uint8_t)((d / maxd) * 255.0f) + (uint8_t)angle;
            m.drawPixel(x + i, y + j, hsv_to_rgb(hue, 255, 200));
        }
}

/* ── SwirlOut ────────────────────────────────────────────────── */
void fx_SwirlOut(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static uint32_t last = 0;
    static uint16_t angle = 0;
    if (_now() - last > (uint32_t)(100 - s->speed * 10)) { last = _now(); angle += 4; }
    float cx = 16, cy = 4;
    float maxd = sqrtf(cx * cx + cy * cy);
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++) {
            float dx = cx - i, dy = cy - j;
            float d = sqrtf(dx * dx + dy * dy);
            uint8_t hue = 255 - (uint8_t)((d / maxd) * 255.0f) + (uint8_t)angle;
            m.drawPixel(x + i, y + j, hsv_to_rgb(hue, 255, 200));
        }
}

/* ── ColorWaves ──────────────────────────────────────────────── */
void fx_ColorWaves(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    float colIdx = 255.0f / 31.0f;
    uint32_t t = (uint32_t)((uint64_t)_now() * (uint32_t)s->speed / 100ULL);
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++) {
            uint8_t pi = (uint8_t)((uint32_t)(i * colIdx + t)) & 0xFF;
            m.drawPixel(x + i, y + j, hsv_to_rgb(pi, 255, 200));
        }
}

/* ── TwinklingStars ──────────────────────────────────────────── */
void fx_TwinklingStars(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static CRGB grid[32][8] = {};
    static uint32_t last = 0;
    if (_now() - last > (uint32_t)(120 - s->speed * 10)) {
        last = _now();
        fadeAllBy(grid, 40);
        for (int k = 0; k < (int)s->speed + 1; k++) {
            int rx = std::rand() % 32, ry = std::rand() % 8;
            grid[rx][ry] = hsv_to_rgb((uint8_t)(std::rand() & 0xFF), 200, 230);
        }
    }
    blitGrid(m, x, y, grid);
}

/* ── LookingEyes (simplified) ────────────────────────────────── */
void fx_LookingEyes(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static uint32_t last = 0;
    static int dir = 0;
    if (_now() - last > (uint32_t)(800 - s->speed * 50)) { last = _now(); dir = std::rand() % 3 - 1; }
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++) m.drawPixel(x + i, y + j, CRGB(0, 0, 0));
    /* two 3x3 eyes; pupil shifts according to dir */
    int leftCX = 9, rightCX = 22, cy = 3;
    int pupilX = (dir == 0) ? 0 : (dir > 0 ? 1 : -1);
    for (int i = -1; i <= 1; i++)
        for (int j = -1; j <= 1; j++) {
            m.drawPixel(x + leftCX  + i, y + cy + j, CRGB(255, 255, 255));
            m.drawPixel(x + rightCX + i, y + cy + j, CRGB(255, 255, 255));
        }
    m.drawPixel(x + leftCX  + pupilX, y + cy, CRGB(0, 0, 0));
    m.drawPixel(x + rightCX + pupilX, y + cy, CRGB(0, 0, 0));
}

/* ── SnakeGame (autonomous snake) ────────────────────────────── */
void fx_SnakeGame(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    struct P { int x, y; };
    static P body[64];
    static int len = 5;
    static int hx = 16, hy = 4;
    static int dx = 1, dy = 0;
    static uint32_t last = 0;
    static int foodX = -1, foodY = -1;
    if (foodX < 0) { foodX = std::rand() % 32; foodY = std::rand() % 8; }
    if (_now() - last > (uint32_t)(180 - s->speed * 15)) {
        last = _now();
        /* choose direction biased toward food */
        if (std::rand() % 3 == 0) {
            int ddx = (foodX > hx) - (foodX < hx);
            int ddy = (foodY > hy) - (foodY < hy);
            if (ddx) { dx = ddx; dy = 0; } else if (ddy) { dy = ddy; dx = 0; }
        }
        for (int i = len - 1; i > 0; i--) body[i] = body[i - 1];
        body[0] = { hx, hy };
        hx += dx; hy += dy;
        if (hx < 0) hx = 31;
        if (hx > 31) hx = 0;
        if (hy < 0) hy = 7;
        if (hy > 7)  hy = 0;
        if (hx == foodX && hy == foodY) {
            if (len < 64) len++;
            foodX = std::rand() % 32;
            foodY = std::rand() % 8;
        }
    }
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++) m.drawPixel(x + i, y + j, CRGB(0, 0, 0));
    m.drawPixel(x + foodX, y + foodY, CRGB(255, 40, 40));
    m.drawPixel(x + hx, y + hy, CRGB(40, 255, 40));
    for (int i = 0; i < len; i++) m.drawPixel(x + body[i].x, y + body[i].y, CRGB(20, 180, 20));
}

/* ── Fireworks ───────────────────────────────────────────────── */
void fx_Fireworks(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static CRGB grid[32][8] = {};
    static uint32_t last = 0;
    if (_now() - last > (uint32_t)(60 - s->speed * 4)) {
        last = _now();
        fadeAllBy(grid, 60);
        if ((std::rand() & 0xFF) < 80) {
            int cx = std::rand() % 32, cy = 1 + std::rand() % 6;
            CRGB c = hsv_to_rgb((uint8_t)(std::rand() & 0xFF), 240, 255);
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++) {
                    int xx = cx + dx, yy = cy + dy;
                    if (xx >= 0 && xx < 32 && yy >= 0 && yy < 8) grid[xx][yy] = c;
                }
        }
    }
    blitGrid(m, x, y, grid);
}

/* ── Ripple ──────────────────────────────────────────────────── */
void fx_Ripple(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static uint32_t last = 0;
    static int cx = 16, cy = 4;
    static float r = 0;
    if (_now() - last > (uint32_t)(50 - s->speed * 4)) {
        last = _now();
        r += 0.6f;
        if (r > 18) { r = 0; cx = std::rand() % 32; cy = std::rand() % 8; }
    }
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++) {
            float dx = i - cx, dy = j - cy;
            float d = sqrtf(dx * dx + dy * dy);
            float diff = fabsf(d - r);
            uint8_t v = (diff < 1.0f) ? 220 : (diff < 2.0f ? 110 : 0);
            m.drawPixel(x + i, y + j, hsv_to_rgb((uint8_t)(r * 12), 220, v));
        }
}

/* ── PlasmaCloud ─────────────────────────────────────────────── */
void fx_PlasmaCloud(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static float t = 0;
    t += 0.05f * (float)s->speed;
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++) {
            float v = sinf(i * 0.2f + t)
                    + sinf((j + i) * 0.15f + t * 0.5f)
                    + sinf(sqrtf((i - 16.0f) * (i - 16.0f) + (j - 4.0f) * (j - 4.0f)) * 0.3f + t);
            uint8_t h = (uint8_t)((v + 3.0f) * 42.0f);
            m.drawPixel(x + i, y + j, hsv_to_rgb(h, 200, 200));
        }
}

/* ── Checkerboard ────────────────────────────────────────────── */
void fx_Checkerboard(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static uint32_t last = 0;
    static int flip = 0;
    if (_now() - last > (uint32_t)(400 - s->speed * 30)) { last = _now(); flip ^= 1; }
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++) {
            bool a = ((i / 2 + j / 2 + flip) & 1) == 0;
            m.drawPixel(x + i, y + j, a ? CRGB(20, 80, 200) : CRGB(220, 30, 90));
        }
}

/* ── Radar ───────────────────────────────────────────────────── */
void fx_Radar(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static CRGB grid[32][8] = {};
    static float angle = 0;
    angle += 0.05f * (float)s->speed;
    if (angle > 2 * M_PI) angle -= 2 * M_PI;
    fadeAllBy(grid, 12);
    for (float r = 0; r < 18; r += 0.5f) {
        int px = (int)(16 + cosf(angle) * r);
        int py = (int)(4  + sinf(angle) * r);
        if (px >= 0 && px < 32 && py >= 0 && py < 8) grid[px][py] = CRGB(0, 255, 0);
    }
    blitGrid(m, x, y, grid);
}

/* ── PingPong ────────────────────────────────────────────────── */
void fx_PingPong(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static float bx = 16, by = 4, vx = 0.7f, vy = 0.35f;
    static int pad1 = 3, pad2 = 3;
    static uint32_t last = 0;
    uint32_t period = (uint32_t)(60 - s->speed * 4);
    if (_now() - last > period) {
        last = _now();
        bx += vx; by += vy;
        if (by <= 0 || by >= 7) vy = -vy;
        if (bx <= 1)  { vx = -vx; pad1 = (int)by; }
        if (bx >= 30) { vx = -vx; pad2 = (int)by; }
    }
    for (int i = 0; i < 32; i++) for (int j = 0; j < 8; j++) m.drawPixel(x + i, y + j, CRGB(0, 0, 0));
    for (int j = -1; j <= 1; j++) {
        if (pad1 + j >= 0 && pad1 + j < 8) m.drawPixel(x + 0,  y + pad1 + j, CRGB(255, 80, 80));
        if (pad2 + j >= 0 && pad2 + j < 8) m.drawPixel(x + 31, y + pad2 + j, CRGB(80, 80, 255));
    }
    m.drawPixel(x + (int)bx, y + (int)by, CRGB(255, 255, 255));
}

/* ── BrickBreaker ────────────────────────────────────────────── */
void fx_BrickBreaker(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static bool bricks[16][2];
    static bool inited = false;
    static float bx = 16, by = 6, vx = 0.6f, vy = -0.4f;
    static int pad = 14;
    static uint32_t last = 0;
    if (!inited) { for (int i = 0; i < 16; i++) for (int j = 0; j < 2; j++) bricks[i][j] = true; inited = true; }
    uint32_t period = (uint32_t)(60 - s->speed * 4);
    if (_now() - last > period) {
        last = _now();
        bx += vx; by += vy;
        if (bx <= 0 || bx >= 31) vx = -vx;
        if (by <= 0) vy = -vy;
        if (by >= 7 && (int)bx >= pad && (int)bx < pad + 4) vy = -vy;
        else if (by >= 7) { /* reset */
            for (int i = 0; i < 16; i++) for (int j = 0; j < 2; j++) bricks[i][j] = true;
            bx = 16; by = 6;
        }
        /* brick collision */
        int bi = (int)bx / 2, bj = (int)by / 2;
        if (bj >= 0 && bj < 2 && bi >= 0 && bi < 16 && bricks[bi][bj]) {
            bricks[bi][bj] = false; vy = -vy;
        }
        pad += (((int)bx - pad - 2) > 0) ? 1 : -1;
        if (pad < 0) pad = 0;
        if (pad > 28) pad = 28;
    }
    for (int i = 0; i < 32; i++) for (int j = 0; j < 8; j++) m.drawPixel(x + i, y + j, CRGB(0, 0, 0));
    for (int bi = 0; bi < 16; bi++)
        for (int bj = 0; bj < 2; bj++)
            if (bricks[bi][bj]) {
                CRGB c = hsv_to_rgb((uint8_t)(bi * 16), 255, 200);
                m.drawPixel(x + bi * 2,     y + bj, c);
                m.drawPixel(x + bi * 2 + 1, y + bj, c);
            }
    for (int k = 0; k < 4; k++) m.drawPixel(x + pad + k, y + 7, CRGB(200, 200, 200));
    m.drawPixel(x + (int)bx, y + (int)by, CRGB(255, 255, 255));
}

/* ── MovingLine ──────────────────────────────────────────────── */
void fx_MovingLine(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static float pos = 0;
    static uint32_t last = 0;
    if (_now() - last > (uint32_t)(50 - s->speed * 4)) { last = _now(); pos += 0.5f; if (pos >= 32) pos = 0; }
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++) {
            int diff = (i - (int)pos + 32) % 32;
            uint8_t v = (diff < 4) ? (uint8_t)(255 - diff * 60) : 0;
            m.drawPixel(x + i, y + j, hsv_to_rgb((uint8_t)((int)pos * 6), 220, v));
        }
}

/* ── Fade (solid color rotating in HSV) ──────────────────────── */
void fx_Fade(Matrix &m, int16_t x, int16_t y, EffectSettings *s) {
    static uint8_t hue = 0;
    static uint32_t last = 0;
    if (_now() - last > (uint32_t)(40 - s->speed * 3)) { last = _now(); hue++; }
    CRGB c = hsv_to_rgb(hue, 255, 200);
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++)
            m.drawPixel(x + i, y + j, c);
}

/* ── Registry ────────────────────────────────────────────────── */
const Effect g_effects[EFFECT_COUNT] = {
    {"Pacifica",       fx_Pacifica,       {2.0, 0, false}},
    {"Plasma",         fx_Plasma,         {2.0, 0, false}},
    {"Rainbow",        fx_Rainbow,        {2.0, 0, false}},
    {"TheaterChase",   fx_TheaterChase,   {5.0, 0, false}},
    {"Sparkles",       fx_Sparkles,       {3.0, 0, false}},
    {"Noise",          fx_Noise,          {1.0, 0, false}},
    {"Rain",           fx_Rain,           {3.0, 0, false}},
    {"Matrix",         fx_Matrix,         {3.0, 0, false}},
    {"SwirlIn",        fx_SwirlIn,        {3.0, 0, false}},
    {"SwirlOut",       fx_SwirlOut,       {3.0, 0, false}},
    {"ColorWaves",     fx_ColorWaves,     {3.0, 0, false}},
    {"TwinklingStars", fx_TwinklingStars, {3.0, 0, false}},
    {"LookingEyes",    fx_LookingEyes,    {3.0, 0, false}},
    {"SnakeGame",      fx_SnakeGame,      {4.0, 0, false}},
    {"Fireworks",      fx_Fireworks,      {3.0, 0, false}},
    {"Ripple",         fx_Ripple,         {3.0, 0, false}},
    {"PlasmaCloud",    fx_PlasmaCloud,    {2.0, 0, false}},
    {"Checkerboard",   fx_Checkerboard,   {3.0, 0, false}},
    {"Radar",          fx_Radar,          {3.0, 0, false}},
    {"PingPong",       fx_PingPong,       {4.0, 0, false}},
    {"BrickBreaker",   fx_BrickBreaker,   {4.0, 0, false}},
};

void callEffect(Matrix &m, int16_t x, int16_t y, int index) {
    if (index >= 0 && index < EFFECT_COUNT)
        g_effects[index].func(m, x, y, (EffectSettings *)&g_effects[index].settings);
}

int getEffectIndex(const char *name) {
    for (int i = 0; i < EFFECT_COUNT; i++)
        if (strcmp(g_effects[i].name, name) == 0) return i;
    return -1;
}

void updateEffectSettings(int index, const char *json) {
    if (index < 0 || index >= EFFECT_COUNT) return;
    cJSON *doc = cJSON_Parse(json);
    if (!doc) return;
    EffectSettings *es = (EffectSettings *)&g_effects[index].settings;
    cJSON *sp = cJSON_GetObjectItem(doc, "speed");
    if (sp && cJSON_IsNumber(sp)) es->speed = sp->valuedouble;
    cJSON *pa = cJSON_GetObjectItem(doc, "palette");
    if (pa && cJSON_IsNumber(pa)) es->palette = pa->valueint;
    cJSON *bl = cJSON_GetObjectItem(doc, "blend");
    if (bl && cJSON_IsBool(bl))  es->blend = cJSON_IsTrue(bl);
    cJSON_Delete(doc);
}

const char *getEffectNames() {
    static char buf[512];
    static bool built = false;
    if (built) return buf;
    char *p = buf; *p++ = '[';
    for (int i = 0; i < EFFECT_COUNT; i++) {
        int n = snprintf(p, sizeof(buf) - (p - buf), "%s\"%s\"", i ? "," : "", g_effects[i].name);
        if (n <= 0) break;
        p += n;
    }
    *p++ = ']'; *p = 0;
    built = true;
    return buf;
}

/* ── Weather Overlays ────────────────────────────────────────────
 * Faithful port of the original AWTRIX3 effects.cpp::EffectOverlay().
 * Maintains a private 32x8 LED grid with column-wise vertical scrolling
 * for rain-style precipitation, additional row-wise wind shift for storms,
 * a randomly-twinkling palette pattern for frost, and a probabilistic
 * white flash overlay for thunder. */

#include "awtrix_globals.h"     /* for the OverlayEffect enum constants */

static inline uint8_t _r8() { return (uint8_t)(std::rand() & 0xFF); }

/* OceanColors-equivalent palette (8 entries) — picked to approximate
 * FastLED's OceanColors_p which the original Arduino code used for FROST. */
static CRGB ocean_palette_lookup(uint8_t v) {
    static const CRGB pal[8] = {
        CRGB(  0,  20,  60),
        CRGB(  0,  60, 110),
        CRGB( 10, 100, 160),
        CRGB( 30, 150, 200),
        CRGB( 80, 200, 220),
        CRGB(140, 220, 230),
        CRGB( 60, 180, 200),
        CRGB( 10, 110, 160),
    };
    int idx = (v * 8) >> 8;
    if (idx < 0) idx = 0;
    if (idx > 7) idx = 7;
    return pal[idx];
}

OverlayEffect parseOverlayName(const char *name) {
    if (!name || !*name) return OVERLAY_NONE;
    /* case-insensitive compare */
    auto eq = [](const char *a, const char *b) {
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) return false;
            a++; b++;
        }
        return *a == 0 && *b == 0;
    };
    if (eq(name, "drizzle")) return OVERLAY_DRIZZLE;
    if (eq(name, "rain"))    return OVERLAY_RAIN;
    if (eq(name, "snow"))    return OVERLAY_SNOW;
    if (eq(name, "storm"))   return OVERLAY_STORM;
    if (eq(name, "thunder")) return OVERLAY_THUNDER;
    if (eq(name, "frost"))   return OVERLAY_FROST;
    if (eq(name, "clear"))   return OVERLAY_NONE;
    if (eq(name, "time"))    return OVERLAY_TIME;
    return OVERLAY_NONE;
}

void EffectOverlay(Matrix &m, int16_t x, int16_t y, int effect) {
    if (effect <= OVERLAY_NONE || effect >= OVERLAY_COUNT) return;
    if (effect == OVERLAY_TIME) return;     /* TIME overlay is rendered elsewhere */

    static CRGB    leds[32][8] = {};
    static uint8_t colorChanges[32][8] = {0};
    static bool          lightning            = false;
    static uint64_t      lastLightningMillis  = 0;
    static const uint32_t lightningDuration   = 50;
    static int           updateFrame          = 0;
    static int           windFrame            = 0;

    /* Lightning logic for THUNDER */
    if (effect == OVERLAY_THUNDER) {
        if (_r8() < 1) {
            lightning = true;
            lastLightningMillis = _now();
        } else if (lightning && (_now() - lastLightningMillis > lightningDuration)) {
            lightning = false;
        }
    }

    /* Generate weather drops */
    int  rainChance = (effect == OVERLAY_STORM || effect == OVERLAY_THUNDER) ? 60
                    : (effect == OVERLAY_DRIZZLE ? 15 : 50);
    CRGB dropColor  = (effect == OVERLAY_DRIZZLE) ? hsv_to_rgb(160, 255, 230)
                                                  : hsv_to_rgb(160, 255, 200);

    if (effect == OVERLAY_RAIN || effect == OVERLAY_STORM ||
        effect == OVERLAY_THUNDER || effect == OVERLAY_DRIZZLE) {
        if (_r8() < rainChance) {
            int rc = std::rand() % 32;
            leds[rc][0] = dropColor;
        }
    } else if (effect == OVERLAY_SNOW && _r8() < 20) {
        int rc = std::rand() % 32;
        leds[rc][0] = CRGB(255, 255, 255);
    }

    /* FROST: twinkling palette pixels */
    if (effect == OVERLAY_FROST) {
        for (int i = 0; i < 32; i++) {
            for (int j = 0; j < 8; j++) {
                if (colorChanges[i][j] > 0) {
                    leds[i][j] = ocean_palette_lookup(_r8());
                    colorChanges[i][j]++;
                    if (colorChanges[i][j] > 6) colorChanges[i][j] = 0;
                }
                if (colorChanges[i][j] == 0) leds[i][j] = CRGB(0, 0, 0);
            }
        }
        if (_r8() < 25) {
            int i = std::rand() % 32;
            int j = std::rand() % 8;
            if (colorChanges[i][j] == 0) {
                colorChanges[i][j] = 1;
                leds[i][j] = ocean_palette_lookup(_r8());
            }
        }
    }

    /* Vertical fall step (every 2 frames, or 5 for snow) */
    if (effect != OVERLAY_FROST && ++updateFrame >= (effect == OVERLAY_SNOW ? 5 : 2)) {
        for (int i = 0; i < 32; i++) {
            for (int j = 7; j > 0; j--) leds[i][j] = leds[i][j - 1];
            leds[i][0] = CRGB(0, 0, 0);
        }
        updateFrame = 0;
    }

    /* Horizontal wind shift for storm/thunder */
    if ((effect == OVERLAY_STORM || effect == OVERLAY_THUNDER) && ++windFrame >= 3) {
        for (int j = 0; j < 8; j++) {
            for (int i = 31; i > 0; i--) leds[i][j] = leds[i - 1][j];
            leds[0][j] = CRGB(0, 0, 0);
        }
        windFrame = 0;
    }

    /* Blit. Lightning overrides every pixel for THUNDER. */
    for (int i = 0; i < 32; i++) {
        for (int j = 0; j < 8; j++) {
            if (lightning && effect == OVERLAY_THUNDER) {
                m.drawPixel(x + i, y + j, CRGB(255, 255, 255));
            } else if (leds[i][j].r || leds[i][j].g || leds[i][j].b) {
                m.drawPixel(x + i, y + j, leds[i][j]);
            }
        }
    }
}