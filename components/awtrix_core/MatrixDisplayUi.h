#pragma once

#ifdef __cplusplus

#include "awtrix_hal.h"
#include "matrix_cpp.h"
#include "awtrix_globals.h"
#include <vector>
#include <map>
#include <string>
#include <functional>
#include <cstdint>

/* ── Forward declarations ──────────────────────────────────── */
class GifPlayer;

/* ── UI state ───────────────────────────────────────────────── */
struct UiState
{
    uint64_t lastUpdate = 0;
    long ticksSinceLastStateSwitch = 0;

    enum AppState { IN_TRANSITION, FIXED };

    AppState appState = FIXED;
    uint8_t currentApp = 0;
    int8_t appTransitionDirection = 1;
    bool lastFrameShown = false;
    bool manualControl = false;
    void* userData = nullptr;
};

/* ── Transition type ────────────────────────────────────────── */
enum TransitionType
{
    TRANS_RANDOM, TRANS_SLIDE, TRANS_FADE, TRANS_ZOOM,
    TRANS_ROTATE, TRANS_PIXELATE, TRANS_CURTAIN, TRANS_RIPPLE,
    TRANS_BLINK, TRANS_RELOAD, TRANS_CROSSFADE, TRANS_COUNT
};

/* ── Animation direction ───────────────────────────────────── */
enum AnimDir { SLIDE_UP, SLIDE_DOWN };

/* ── Callback signatures ───────────────────────────────────── */
using AppCallback = std::function<void(Matrix &, UiState &, int16_t, int16_t, GifPlayer *)>;
using OverlayCallback = std::function<void(Matrix &, UiState &, GifPlayer *)>;
using BackgroundCallback = std::function<void(Matrix &)>;

/* ── MatrixDisplayUi class ──────────────────────────────────── */
class MatrixDisplayUi
{
public:
    explicit MatrixDisplayUi(Matrix* matrix);
    void init();
    void setTargetFPS(uint8_t fps);
    void setTimePerApp(long ms);
    void setTimePerTransition(uint16_t ms);
    void setAppAnimation(AnimDir dir);
    void setBackgroundEffect(int effect);

    /* app manager */
    void setApps(const std::vector<std::pair<std::string, AppCallback>>& appPairs);
    uint8_t getAppCount() const { return m_appCount; }

    /* overlay manager */
    void setOverlays(const std::vector<OverlayCallback>& overlays);
    void setOverlayCount(uint8_t count);

    /* auto transition */
    void enableAutoTransition();
    void disableAutoTransition();
    void setAutoTransitionForwards();
    void setAutoTransitionBackwards();

    /* manual control */
    void nextApp();
    void previousApp();
    bool switchToApp(uint8_t app);
    void transitionToApp(uint8_t app);
    uint8_t getNextAppNumber() const;
    void forceResetState();

    /* indicators */
    void setIndicator1Color(uint32_t color) { m_ind1Color = color; }
    void setIndicator1State(bool state) { m_ind1State = state; }
    void setIndicator1Blink(int b) { m_ind1Blink = b; }
    void setIndicator1Fade(int f) { m_ind1Fade = f; }
    void setIndicator2Color(uint32_t color) { m_ind2Color = color; }
    void setIndicator2State(bool state) { m_ind2State = state; }
    void setIndicator2Blink(int b) { m_ind2Blink = b; }
    void setIndicator2Fade(int f) { m_ind2Fade = f; }
    void setIndicator3Color(uint32_t color) { m_ind3Color = color; }
    void setIndicator3State(bool state) { m_ind3State = state; }
    void setIndicator3Blink(int b) { m_ind3Blink = b; }
    void setIndicator3Fade(int f) { m_ind3Fade = f; }

    /* tick */
    int8_t update();

    UiState* getUiState() { return &m_state; }
    Matrix* getMatrix() { return m_matrix; }

private:
    void tick();
    void drawApp();
    void drawOverlays();
    void drawIndicators();
    uint32_t fadeColor(uint32_t color, uint32_t interval);
    void doTransition();
    void drawBackground();

    Matrix* m_matrix;
    UiState m_state;

    AnimDir m_animDir = SLIDE_DOWN;
    int8_t m_lastTransDir = 1;

    long m_ticksPerApp = 150;
    uint16_t m_ticksPerTrans = 15;
    long m_updateInterval = 33;

    bool m_autoTrans = true;
    int8_t m_nextAppNum = -1;

    AppCallback* m_appFuncs = nullptr;
    uint8_t m_appCount = 0;
    OverlayCallback* m_overlayFuncs = nullptr;
    uint8_t m_overlayCount = 0;
    BackgroundCallback m_bgFunc = nullptr;
    int m_bgEffect = -1;

    /* indicators */
    uint32_t m_ind1Color = 0xFF0000, m_ind2Color = 0x00FF00, m_ind3Color = 0x0000FF;
    bool m_ind1State = false, m_ind2State = false, m_ind3State = false;
    int m_ind1Blink = 0, m_ind2Blink = 0, m_ind3Blink = 0;
    int m_ind1Fade = 0, m_ind2Fade = 0, m_ind3Fade = 0;
};

#endif /* __cplusplus */