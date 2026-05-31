#pragma once

#ifdef __cplusplus

#include "matrix_cpp.h"
#include <stdint.h>

/* ── Effect settings ─────────────────────────────────────────  */
struct EffectSettings
{
    double speed = 2.0;
    int palette = 0; /* index into palette list */
    bool blend = false;
};

/* ── Effect function signature ──────────────────────────────── */
using EffectFunc = void (*)(Matrix&, int16_t x, int16_t y, EffectSettings*);

/* ── Effect descriptor ──────────────────────────────────────── */
struct Effect
{
    const char* name;
    EffectFunc func;
    EffectSettings settings;
};

/* ── Registry ────────────────────────────────────────────────── */
enum { EFFECT_COUNT = 21 };

extern const Effect g_effects[EFFECT_COUNT];

void callEffect(Matrix& m, int16_t x, int16_t y, int index);
int getEffectIndex(const char* name);

/* ── Individual effects ──────────────────────────────────────── */
void fx_Pacifica(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_Plasma(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_Rainbow(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_TheaterChase(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_Sparkles(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_Noise(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_Rain(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_Matrix(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_SwirlIn(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_SwirlOut(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_ColorWaves(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_TwinklingStars(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_LookingEyes(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_SnakeGame(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_Fireworks(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_Ripple(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_PlasmaCloud(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_Checkerboard(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_Radar(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_PingPong(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_BrickBreaker(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_MovingLine(Matrix& m, int16_t x, int16_t y, EffectSettings* s);
void fx_Fade(Matrix& m, int16_t x, int16_t y, EffectSettings* s);

/* JSON-driven settings updater (called from /api/effects POST body, mirrors
 * the original updateEffectSettings()). Body shape: {"name":"Pacifica","speed":2,...}. */
void updateEffectSettings(int index, const char* json);
const char* getEffectNames(); /* JSON array string, cached */

/* ── Weather overlays ────────────────────────────────────────────
 * Renders one of the 6 weather animations (drizzle/rain/snow/storm/
 * thunder/frost) at the (x,y) origin. Mirrors the original
 * AWTRIX3 effects.cpp::EffectOverlay(). The OverlayEffect enum lives
 * in awtrix_globals.h; we accept a plain `int` here so this header
 * stays self-contained without a forward enum declaration. */
void EffectOverlay(Matrix& m, int16_t x, int16_t y, int effect /* OverlayEffect */);

#endif /* __cplusplus */