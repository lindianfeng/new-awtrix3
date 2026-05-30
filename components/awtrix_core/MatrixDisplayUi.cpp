#include "MatrixDisplayUi.h"
#include "effects_core.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "ui"

/* dummy GifPlayer placeholder */
class GifPlayer {
public:
    void setMatrix(Matrix *) {}
};

/* ── Helper: current time in ms ────────────────────────────── */
static uint64_t now_ms() { return esp_timer_get_time() / 1000; }

/* ── Constructor ────────────────────────────────────────────── */
MatrixDisplayUi::MatrixDisplayUi(Matrix *matrix) : m_matrix(matrix) {}

void MatrixDisplayUi::init() {
    m_state.lastUpdate = now_ms();
}

void MatrixDisplayUi::setTargetFPS(uint8_t fps) {
    m_updateInterval = (long)((1.0f / (float)fps) * 1000.0f);
    /* setTimePerTransition() must be called AFTER setTargetFPS to recalc m_ticksPerTrans */
}

void MatrixDisplayUi::setTimePerApp(long ms) {
    m_ticksPerApp = m_updateInterval > 0 ? ms / m_updateInterval : 150;
}

void MatrixDisplayUi::setTimePerTransition(uint16_t ms) {
    m_ticksPerTrans = m_updateInterval > 0 ? (uint16_t)((float)ms / (float)m_updateInterval) : 15;
}

void MatrixDisplayUi::setAppAnimation(AnimDir dir) { m_animDir = dir; }
void MatrixDisplayUi::setBackgroundEffect(int effect) {
    m_bgEffect = effect;
    if (effect >= 0 && effect < EFFECT_COUNT) {
        m_bgFunc = [this](Matrix &m) {
            callEffect(m, 0, 0, m_bgEffect);
        };
    } else {
        m_bgFunc = nullptr;
    }
}

void MatrixDisplayUi::enableAutoTransition()         { m_autoTrans = true; }
void MatrixDisplayUi::disableAutoTransition()        { m_autoTrans = false; }
void MatrixDisplayUi::setAutoTransitionForwards()    { m_state.appTransitionDirection = 1; m_lastTransDir = 1; }
void MatrixDisplayUi::setAutoTransitionBackwards()   { m_state.appTransitionDirection = -1; m_lastTransDir = -1; }

void MatrixDisplayUi::setOverlays(const std::vector<OverlayCallback> &overlays) {
    delete[] m_overlayFuncs;
    m_overlayCount = overlays.size();
    if (m_overlayCount > 0) {
        m_overlayFuncs = new OverlayCallback[m_overlayCount];
        for (size_t i = 0; i < m_overlayCount; i++) m_overlayFuncs[i] = overlays[i];
    }
}

void MatrixDisplayUi::setOverlayCount(uint8_t cnt) { m_overlayCount = cnt; }

/* ── Apps ───────────────────────────────────────────────────── */
void MatrixDisplayUi::setApps(const std::vector<std::pair<std::string, AppCallback>> &appPairs) {
    delete[] m_appFuncs;
    m_appCount = appPairs.size();
    m_appFuncs = new AppCallback[m_appCount];
    for (size_t i = 0; i < m_appCount; i++) m_appFuncs[i] = appPairs[i].second;
    forceResetState();
}

void MatrixDisplayUi::nextApp() {
    if (m_state.appState != UiState::IN_TRANSITION) {
        m_state.manualControl = true;
        m_state.appState = UiState::IN_TRANSITION;
        m_state.ticksSinceLastStateSwitch = 0;
        m_lastTransDir = m_state.appTransitionDirection;
        m_state.appTransitionDirection = 1;
    }
}

void MatrixDisplayUi::previousApp() {
    if (m_state.appState != UiState::IN_TRANSITION) {
        m_state.manualControl = true;
        m_state.appState = UiState::IN_TRANSITION;
        m_state.ticksSinceLastStateSwitch = 0;
        m_lastTransDir = m_state.appTransitionDirection;
        m_state.appTransitionDirection = -1;
    }
}

bool MatrixDisplayUi::switchToApp(uint8_t app) {
    if (app >= m_appCount) return false;
    m_state.ticksSinceLastStateSwitch = 0;
    if (app == m_state.currentApp) return false;
    m_state.appState = UiState::FIXED;
    m_state.currentApp = app;
    return true;
}

void MatrixDisplayUi::transitionToApp(uint8_t app) {
    if (app >= m_appCount) return;
    m_state.ticksSinceLastStateSwitch = 0;
    if (app == m_state.currentApp) return;
    m_nextAppNum = app;
    m_lastTransDir = m_state.appTransitionDirection;
    m_state.manualControl = true;
    m_state.appState = UiState::IN_TRANSITION;
    m_state.appTransitionDirection = app < m_state.currentApp ? -1 : 1;
}

void MatrixDisplayUi::forceResetState() {
    m_state.currentApp = 0;
    m_state.ticksSinceLastStateSwitch = 0;
    m_state.lastUpdate = now_ms();
    m_state.appState = UiState::FIXED;
}

uint8_t MatrixDisplayUi::getNextAppNumber() const {
    if (m_nextAppNum != -1) return m_nextAppNum;
    int8_t next = (int8_t)m_state.currentApp + m_state.appTransitionDirection;
    if (next < 0) next = m_appCount - 1;
    if (next >= (int8_t)m_appCount) next = 0;
    return (uint8_t)next;
}

/* ── Update / tick ──────────────────────────────────────────── */
int8_t MatrixDisplayUi::update() {
    uint64_t now = now_ms();
    long budget = m_updateInterval - (now - m_state.lastUpdate);
    if (budget <= 0) {
        if (m_autoTrans && m_state.lastUpdate != 0)
            m_state.ticksSinceLastStateSwitch += (long)ceil(-budget / (float)m_updateInterval);
        m_state.lastUpdate = now;
        tick();
    }
    return m_updateInterval - (now_ms() - now);
}

void MatrixDisplayUi::tick() {
    m_state.ticksSinceLastStateSwitch++;

    if (m_appCount > 0) {
        switch (m_state.appState) {
        case UiState::IN_TRANSITION:
            if (m_state.ticksSinceLastStateSwitch >= m_ticksPerTrans) {
                m_state.appState = UiState::FIXED;
                m_state.currentApp = getNextAppNumber();
                m_state.ticksSinceLastStateSwitch = 0;
                m_nextAppNum = -1;
            }
            break;
        case UiState::FIXED:
            if (m_state.manualControl) {
                m_state.appTransitionDirection = 1;
                m_state.manualControl = false;
            }
            if (m_autoTrans && m_state.ticksSinceLastStateSwitch >= m_ticksPerApp)
                m_state.appState = UiState::IN_TRANSITION;
            break;
        }
    }

    m_matrix->clear();

    if (m_bgEffect >= 0 && m_bgFunc) drawBackground();

    if (m_appCount > 0) drawApp();
    drawOverlays();
    drawIndicators();
    m_matrix->show();
}

void MatrixDisplayUi::drawBackground() {
    if (m_bgFunc) m_bgFunc(*m_matrix);
}

/* ── Transition effect indices (must match CONFIG.transEffect / web UI) ──
 *  0 = RANDOM, 1 = SLIDE, 2 = FADE, 3 = ZOOM, 4 = ROTATE, 5 = PIXELATE,
 *  6 = CURTAIN, 7 = RIPPLE, 8 = BLINK, 9 = RELOAD, 10 = CROSSFADE
 * Mirrors src/MatrixDisplayUi.h::TransitionType in the original AWTRIX3.
 */
enum {
    TX_RANDOM = 0, TX_SLIDE, TX_FADE, TX_ZOOM, TX_ROTATE,
    TX_PIXELATE, TX_CURTAIN, TX_RIPPLE, TX_BLINK, TX_RELOAD, TX_CROSSFADE,
    TX_COUNT
};

/* Off-screen copy of the previous frame, used by transitions that need to
 * blend or wipe across the old frame. Sized for 32x8. */
static CRGB s_ledsCopy[32 * 8];
static int  s_currentTransition = TX_SLIDE;
static bool s_gotNewTransition  = false;

static int pickRandomTransition() {
    /* Pick a non-RANDOM index in [TX_SLIDE..TX_CROSSFADE]. */
    return (rand() % (TX_COUNT - 1)) + 1;
}

static inline void scaleColor(CRGB &c, float k) {
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    c.r = (uint8_t)((float)c.r * k);
    c.g = (uint8_t)((float)c.g * k);
    c.b = (uint8_t)((float)c.b * k);
}

static inline CRGB lerpColor(const CRGB &a, const CRGB &b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return CRGB(
        (uint8_t)((float)a.r + ((float)b.r - (float)a.r) * t),
        (uint8_t)((float)a.g + ((float)b.g - (float)a.g) * t),
        (uint8_t)((float)a.b + ((float)b.b - (float)a.b) * t)
    );
}

static inline void rotatePoint(int &x, int &y, float angle) {
    /* Rotate around the matrix center (16, 4). Mirrors the original
     * rotateTransition helper. */
    int cx = x - 16, cy = y - 4;
    float ca = cosf(angle), sa = sinf(angle);
    x = (int)(cx * ca - cy * sa) + 16;
    y = (int)(cx * sa + cy * ca) + 4;
}

void MatrixDisplayUi::drawApp() {
    if (m_state.appState == UiState::IN_TRANSITION) {
        /* Resolve the concrete transition once per IN_TRANSITION period. */
        if (!s_gotNewTransition) {
            int eff = CONFIG.transEffect;
            if (eff == TX_RANDOM || eff < 0 || eff >= TX_COUNT) {
                s_currentTransition = pickRandomTransition();
            } else {
                s_currentTransition = eff;
            }
            s_gotNewTransition = true;
        }
        doTransition();
        return;
    }
    /* FIXED — just paint the current app and clear the "new transition"
     * latch so the next IN_TRANSITION re-rolls the effect. */
    s_gotNewTransition = false;
    uint8_t idx = m_state.currentApp;
    if (idx < m_appCount && m_appFuncs[idx]) {
        GifPlayer dummy;
        m_appFuncs[idx](*m_matrix, m_state, 0, 0, &dummy);
    }
}

void MatrixDisplayUi::drawOverlays() {
    if (!m_overlayFuncs) return;
    for (uint8_t i = 0; i < m_overlayCount; i++) {
        if (m_overlayFuncs[i]) {
            GifPlayer dummy;
            m_overlayFuncs[i](*m_matrix, m_state, &dummy);
        }
    }
}

/* ── Helpers used by every transition ──────────────────────────── */
static inline GifPlayer &dummyGif() {
    static GifPlayer g;
    return g;
}

static inline void copyLedsToSnapshot(Matrix *m) {
    int w = m->width(), h = m->height();
    if (w * h > (int)(sizeof(s_ledsCopy) / sizeof(s_ledsCopy[0]))) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            s_ledsCopy[i + j * w] = m->getPixel(i, j);
}

static inline CRGB snapshotPixel(int x, int y, int w) {
    return s_ledsCopy[x + y * w];
}

/* Slide (current app slides out horizontally, next slides in). */
static void txSlide(MatrixDisplayUi *ui, Matrix *matrix, AppCallback *funcs,
                    UiState &state, uint8_t curIdx, uint8_t nextIdx,
                    int ticksPerTransition) {
    (void)ui;
    int dir = state.appTransitionDirection >= 0 ? 1 : -1;
    int w = matrix->width();
    int offset = (state.ticksSinceLastStateSwitch * w) / ticksPerTransition;
    if (funcs[curIdx])  funcs[curIdx] (*matrix, state, -dir * offset, 0, &dummyGif());
    if (funcs[nextIdx]) funcs[nextIdx](*matrix, state,  dir * (w - offset), 0, &dummyGif());
}

/* Fade — fade out old half then fade in new half. */
static void txFade(Matrix *matrix, AppCallback *funcs, UiState &state,
                   uint8_t curIdx, uint8_t nextIdx, int ticksPerTransition) {
    float progress = (float)state.ticksSinceLastStateSwitch / (float)ticksPerTransition;
    matrix->clear();
    if (progress < 0.5f) {
        if (funcs[curIdx]) funcs[curIdx](*matrix, state, 0, 0, &dummyGif());
    } else {
        if (funcs[nextIdx]) funcs[nextIdx](*matrix, state, 0, 0, &dummyGif());
    }
    /* Compute brightness scale: 1.0 at boundaries, 0.0 mid-transition. */
    float k = (progress < 0.5f) ? (1.0f - progress * 2.0f) : ((progress - 0.5f) * 2.0f);
    int w = matrix->width(), h = matrix->height();
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            CRGB c = matrix->getPixel(i, j);
            scaleColor(c, k);
            matrix->drawPixel(i, j, c);
        }
    }
}

/* Zoom — old app shrinks, new app grows. */
static void txZoom(Matrix *matrix, AppCallback *funcs, UiState &state,
                   uint8_t curIdx, uint8_t nextIdx, int ticksPerTransition) {
    float progress = (float)state.ticksSinceLastStateSwitch / (float)ticksPerTransition;
    float scale;
    if (progress < 0.5f) {
        scale = 1.0f - progress * 2.0f;
        if (funcs[curIdx]) funcs[curIdx](*matrix, state, 0, 0, &dummyGif());
    } else {
        scale = (progress - 0.5f) * 2.0f;
        if (funcs[nextIdx]) funcs[nextIdx](*matrix, state, 0, 0, &dummyGif());
    }
    int w = matrix->width(), h = matrix->height();
    copyLedsToSnapshot(matrix);
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int is = (int)(16 + (float)(i - 16) * scale);
            int js = (int)(4  + (float)(j - 4)  * scale);
            if (is < 0) is = 0;
            if (is >= w) is = w - 1;
            if (js < 0) js = 0;
            if (js >= h) js = h - 1;
            matrix->drawPixel(i, j, snapshotPixel(is, js, w));
        }
    }
}

/* Rotate — full 360° spin (sample after rotating each pixel back). */
static void txRotate(Matrix *matrix, AppCallback *funcs, UiState &state,
                     uint8_t curIdx, uint8_t nextIdx, int ticksPerTransition) {
    float progress = (float)state.ticksSinceLastStateSwitch / (float)ticksPerTransition;
    float angle = progress * 2.0f * (float)M_PI;
    if (progress < 0.5f) {
        if (funcs[curIdx]) funcs[curIdx](*matrix, state, 0, 0, &dummyGif());
    } else {
        if (funcs[nextIdx]) funcs[nextIdx](*matrix, state, 0, 0, &dummyGif());
    }
    int w = matrix->width(), h = matrix->height();
    copyLedsToSnapshot(matrix);
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int is = i, js = j;
            rotatePoint(is, js, angle);
            if (is < 0) is = 0;
            if (is >= w) is = w - 1;
            if (js < 0) js = 0;
            if (js >= h) js = h - 1;
            matrix->drawPixel(i, j, snapshotPixel(is, js, w));
        }
    }
}

/* Pixelate — randomly swap pixels from old → new as progress grows. */
static void txPixelate(Matrix *matrix, AppCallback *funcs, UiState &state,
                       uint8_t curIdx, uint8_t nextIdx, int ticksPerTransition) {
    float progress = (float)state.ticksSinceLastStateSwitch / (float)ticksPerTransition;
    if (funcs[curIdx]) funcs[curIdx](*matrix, state, 0, 0, &dummyGif());
    copyLedsToSnapshot(matrix);
    matrix->clear();
    if (funcs[nextIdx]) funcs[nextIdx](*matrix, state, 0, 0, &dummyGif());
    int w = matrix->width(), h = matrix->height();
    int threshold = (int)(progress * 255.0f);
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if ((rand() & 0xFF) > threshold) {
                matrix->drawPixel(i, j, snapshotPixel(i, j, w));
            }
        }
    }
}

/* Curtain — opens a vertical gap from the center, revealing the new app. */
static void txCurtain(Matrix *matrix, AppCallback *funcs, UiState &state,
                      uint8_t curIdx, uint8_t nextIdx, int ticksPerTransition) {
    float progress = (float)state.ticksSinceLastStateSwitch / (float)ticksPerTransition;
    int w = matrix->width(), h = matrix->height();
    int curtainWidth = (int)((float)(w / 2) * progress);

    /* First few ticks: snapshot the old app once. */
    if (state.ticksSinceLastStateSwitch <= 1) {
        if (funcs[curIdx]) funcs[curIdx](*matrix, state, 0, 0, &dummyGif());
        copyLedsToSnapshot(matrix);
    }
    /* Draw new frame. */
    matrix->clear();
    if (funcs[nextIdx]) funcs[nextIdx](*matrix, state, 0, 0, &dummyGif());

    /* Cover the still-curtained columns with the snapshot. */
    int center = w / 2;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (i < (center - curtainWidth) || i >= (center + curtainWidth)) {
                matrix->drawPixel(i, j, snapshotPixel(i, j, w));
            }
        }
    }
}

/* Ripple — checkerboard wipe (even pixels first, then odd). */
static void txRipple(Matrix *matrix, AppCallback *funcs, UiState &state,
                     uint8_t curIdx, uint8_t nextIdx, int ticksPerTransition) {
    float progress = (float)state.ticksSinceLastStateSwitch / (float)ticksPerTransition;
    if (funcs[curIdx]) funcs[curIdx](*matrix, state, 0, 0, &dummyGif());
    copyLedsToSnapshot(matrix);
    matrix->clear();
    if (funcs[nextIdx]) funcs[nextIdx](*matrix, state, 0, 0, &dummyGif());
    int w = matrix->width(), h = matrix->height();
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            bool even = ((i + j) & 1) == 0;
            if ((even && progress < 0.5f) || (!even && progress >= 0.5f)) {
                matrix->drawPixel(i, j, snapshotPixel(i, j, w));
            }
        }
    }
}

/* Blink — clear-flash 3 times while crossing the half-way mark. */
static void txBlink(Matrix *matrix, AppCallback *funcs, UiState &state,
                    uint8_t curIdx, uint8_t nextIdx, int ticksPerTransition) {
    float progress = (float)state.ticksSinceLastStateSwitch / (float)ticksPerTransition;
    int blinks = 3;
    bool on = ((int)(progress * blinks) % 2) == 0;
    if (on) {
        if (progress < 0.5f) {
            if (funcs[curIdx])  funcs[curIdx] (*matrix, state, 0, 0, &dummyGif());
        } else {
            if (funcs[nextIdx]) funcs[nextIdx](*matrix, state, 0, 0, &dummyGif());
        }
    } else {
        matrix->clear();
    }
}

/* Reload — old app flies out to the right; new app flies back in. */
static void txReload(Matrix *matrix, AppCallback *funcs, UiState &state,
                     uint8_t curIdx, uint8_t nextIdx, int ticksPerTransition) {
    float progress = (float)state.ticksSinceLastStateSwitch / (float)ticksPerTransition;
    int w = matrix->width(), h = matrix->height();
    int visiblePixel;
    if (progress < 0.5f) {
        if (funcs[curIdx]) funcs[curIdx](*matrix, state, 0, 0, &dummyGif());
        visiblePixel = (int)((float)w * (1.0f - progress * 2.0f));
        if (visiblePixel < 0) visiblePixel = 0;
        for (int i = visiblePixel; i < w; i++)
            for (int j = 0; j < h; j++)
                matrix->drawPixel(i, j, CRGB(0, 0, 0));
    } else {
        if (funcs[nextIdx]) funcs[nextIdx](*matrix, state, 0, 0, &dummyGif());
        visiblePixel = (int)((float)w * ((progress - 0.5f) * 2.0f));
        if (visiblePixel > w) visiblePixel = w;
        for (int i = visiblePixel; i < w; i++)
            for (int j = 0; j < h; j++)
                matrix->drawPixel(i, j, CRGB(0, 0, 0));
    }
}

/* Crossfade — linear blend between snapshot of old and new frame. */
static void txCrossfade(Matrix *matrix, AppCallback *funcs, UiState &state,
                        uint8_t curIdx, uint8_t nextIdx, int ticksPerTransition) {
    float progress = (float)state.ticksSinceLastStateSwitch / (float)ticksPerTransition;
    if (funcs[curIdx]) funcs[curIdx](*matrix, state, 0, 0, &dummyGif());
    copyLedsToSnapshot(matrix);
    matrix->clear();
    if (funcs[nextIdx]) funcs[nextIdx](*matrix, state, 0, 0, &dummyGif());
    int w = matrix->width(), h = matrix->height();
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            CRGB nw = matrix->getPixel(i, j);
            CRGB old = snapshotPixel(i, j, w);
            matrix->drawPixel(i, j, lerpColor(old, nw, progress));
        }
    }
}

void MatrixDisplayUi::doTransition() {
    if (m_appCount == 0) return;
    uint8_t curIdx  = m_state.currentApp;
    uint8_t nextIdx = getNextAppNumber();
    if (curIdx >= m_appCount || nextIdx >= m_appCount) return;

    switch (s_currentTransition) {
        case TX_FADE:      txFade     (m_matrix, m_appFuncs, m_state, curIdx, nextIdx, m_ticksPerTrans); break;
        case TX_ZOOM:      txZoom     (m_matrix, m_appFuncs, m_state, curIdx, nextIdx, m_ticksPerTrans); break;
        case TX_ROTATE:    txRotate   (m_matrix, m_appFuncs, m_state, curIdx, nextIdx, m_ticksPerTrans); break;
        case TX_PIXELATE:  txPixelate (m_matrix, m_appFuncs, m_state, curIdx, nextIdx, m_ticksPerTrans); break;
        case TX_CURTAIN:   txCurtain  (m_matrix, m_appFuncs, m_state, curIdx, nextIdx, m_ticksPerTrans); break;
        case TX_RIPPLE:    txRipple   (m_matrix, m_appFuncs, m_state, curIdx, nextIdx, m_ticksPerTrans); break;
        case TX_BLINK:     txBlink    (m_matrix, m_appFuncs, m_state, curIdx, nextIdx, m_ticksPerTrans); break;
        case TX_RELOAD:    txReload   (m_matrix, m_appFuncs, m_state, curIdx, nextIdx, m_ticksPerTrans); break;
        case TX_CROSSFADE: txCrossfade(m_matrix, m_appFuncs, m_state, curIdx, nextIdx, m_ticksPerTrans); break;
        case TX_SLIDE:
        default:
            txSlide(this, m_matrix, m_appFuncs, m_state, curIdx, nextIdx, m_ticksPerTrans);
            break;
    }
}

void MatrixDisplayUi::drawIndicators() {
    /* Indicator 1 – top right */
    if (m_ind1State) {
        uint32_t col = m_ind1Color;
        if (m_ind1Blink > 0 && (now_ms() % (2 * m_ind1Blink) < (uint64_t)m_ind1Blink)) col = 0;
        if (m_ind1Fade > 0) col = fadeColor(m_ind1Color, m_ind1Fade);
        m_matrix->drawPixel(31, 0, col);
        m_matrix->drawPixel(30, 0, col);
        m_matrix->drawPixel(31, 1, col);
    }
    /* Indicator 2 – mid right */
    if (m_ind2State) {
        uint32_t col = m_ind2Color;
        if (m_ind2Blink > 0 && (now_ms() % (2 * m_ind2Blink) < (uint64_t)m_ind2Blink)) col = 0;
        if (m_ind2Fade > 0) col = fadeColor(m_ind2Color, m_ind2Fade);
        m_matrix->drawPixel(31, 3, col);
        m_matrix->drawPixel(31, 4, col);
    }
    /* Indicator 3 – bottom right */
    if (m_ind3State) {
        uint32_t col = m_ind3Color;
        if (m_ind3Blink > 0 && (now_ms() % (2 * m_ind3Blink) < (uint64_t)m_ind3Blink)) col = 0;
        if (m_ind3Fade > 0) col = fadeColor(m_ind3Color, m_ind3Fade);
        m_matrix->drawPixel(31, 7, col);
        m_matrix->drawPixel(31, 6, col);
        m_matrix->drawPixel(30, 7, col);
    }
}

uint32_t MatrixDisplayUi::fadeColor(uint32_t color, uint32_t interval) {
    float phase = (sinf(2.0f * M_PI * (float)(now_ms() % interval) / (float)interval) + 1.0f) * 0.5f;
    uint8_t r = (uint8_t)(((color >> 16) & 0xFF) * phase);
    uint8_t g = (uint8_t)(((color >> 8) & 0xFF) * phase);
    uint8_t b = (uint8_t)((color & 0xFF) * phase);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}