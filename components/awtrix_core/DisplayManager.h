#pragma once

#ifdef __cplusplus

#include "awtrix_hal.h"
#include "matrix_cpp.h"
#include "MatrixDisplayUi.h"
#include "freertos/FreeRTOS.h"   /* portMUX_TYPE / portENTER_CRITICAL */
#include <map>
#include <vector>
#include <string>

/* ── Compiled drawInstructions opcode table ──────────────────────
 * Pre-parsing the drawInstructions JSON during parseCustomPage and
 * caching the result as a POD vector eliminates the 60-Hz cJSON_Parse
 * heap thrash that the original implementation incurred for every
 * custom-app frame. The JSON string is still kept so we can persist the
 * original payload to SPIFFS round-trip. */
enum DrawOpCode : uint8_t {
    OPC_NONE     = 0,
    OPC_PIXEL,        /* dp:  args=[x,y]            color */
    OPC_LINE,         /* dl:  args=[x0,y0,x1,y1]    color */
    OPC_FILLRECT,     /* db:  args=[x,y,w,h]        color */
    OPC_RECT,         /* dr:  args=[x,y,w,h]        color */
    OPC_FILLCIRCLE,   /* df:  args=[x,y,r]          color */
    OPC_CIRCLE,       /* dc:  args=[x,y,r]          color */
    OPC_TEXT,         /* dt:  args=[x,y]            color, text_idx → texts[] */
    OPC_PALETTE,      /* dlp: args=[count]          paletteEntries[0..count-1] */
    OPC_PALETTE_PIX,  /* dpa: args=[x,y,palette_idx] */
};

struct DrawOp {
    uint8_t  op;          /* DrawOpCode */
    int16_t  a[4];        /* opcode-specific integer args */
    uint32_t color;       /* RGB888 (or palette[0] for OPC_PALETTE) */
    uint8_t  textIdx;     /* index into CustomApp::drawTexts[] for OPC_TEXT */
    uint8_t  paletteSize; /* OPC_PALETTE: how many trailing palette entries are valid */
    uint32_t palette[4];  /* OPC_PALETTE inline mini-palette (extends via subsequent ops) */
};

/* ── Custom app data ───────────────────────────────────────── */
struct CustomApp {
    std::string name;
    std::string text;
    std::string drawInstructions;        /* original JSON, kept for SPIFFS round-trip + getAppsAsJson */
    std::vector<DrawOp>      compiledDraw;  /* parsed once in parseCustomPage, consumed every frame */
    std::vector<std::string> drawTexts;     /* string table referenced by OPC_TEXT::textIdx */
    std::string iconName;
    uint32_t    color = 0xFFFFFF;
    int         effect = -1;
    int         gradient[2] = {0, 0};
    bool        rainbow = false;
    bool        center = false;
    bool        bounce = false;
    bool        topText = true;
    bool        noScrolling = true;
    int         bounceDir = 0;
    int         textCase = 0;
    int         textOffset = 0;
    int         iconOffset = 0;
    int         fade = 0;
    int         blink = 0;
    float       scrollSpeed = 100;
    float       scrollposition = 0;
    float       iconPosition = 0;
    int16_t     scrollDelay = 0;
    int         repeat = -1;
    int16_t     currentRepeat = 0;
    long        duration = 0;
    long        lastUpdate = 0;
    uint64_t    lifetime = 0;
    uint8_t     lifetimeMode = 0;
    bool        lifeTimeEnd = false;
    uint8_t     currentFrame = 0;
    bool        hasCustomColor = false;
    bool        isGif = false;
    uint8_t     pushIcon = 0;
    bool        iconWasPushed = false;
    int         barData[16] = {0};
    uint32_t    barBG = 0;
    int         barSize = 0;
    int         lineData[16] = {0};
    int         lineSize = 0;
    int         progress = -1;
    uint32_t    pColor = 0;
    uint32_t    pbColor = 0;
    uint32_t    background = 0;
    std::vector<uint32_t> colors;
    std::vector<std::string> fragments;
};

/* ── Notification ───────────────────────────────────────────── */
struct AwtrixNotification {
    std::string text;
    uint32_t    color = 0xFFFFFF;
    int         effect = -1;
    int         duration = 5;
    long        startTime = 0;
    uint32_t    bgColor = 0;
    bool        rtttl = false;
    bool        wakeup = false;
    bool        push = false;
    bool        stack = true;

    /* Runtime / rendering state used by NotifyOverlay (mirrors fields
     * read by src/Overlays.cpp::NotifyOverlay in the original). */
    float       scrollPosition = 0.0f;
    bool        soundPlayed    = false;
    std::string sound;            /* raw RTTTL payload if rtttl=true */
    int         repeat          = -1;      /* -1 → time-based eviction */
    bool        noScrolling     = false;
};

/* ── DisplayManager singleton ───────────────────────────────── */
class DisplayManager {
public:
    static DisplayManager &get() {
        static DisplayManager instance;
        return instance;
    }

    void setup(Matrix *m);
    void tick();

    /* app management */
    void loadNativeApps();
    void loadCustomApps();
    void nextApp();
    void previousApp();
    void forceNextApp();
    void leftButton();
    void rightButton();
    void selectButton();
    void selectButtonLong();
    bool switchToApp(const char *json);
    void setAppTime(long ms);

    /* notification */
    bool generateNotification(uint8_t source, const char *json);
    void dismissNotify();

    /* drawing conveniences (Adafruit-GFX-like front-ends that bridge to Matrix) */
    void drawProgressBar(int16_t x, int16_t y, int progress,
                         uint32_t pColor, uint32_t pbColor);
    void HSVtext(int16_t x, int16_t y, const char *text, bool centered, uint8_t textCase);
    void printText(int16_t x, int16_t y, const char *text, bool centered, uint8_t textCase);
    void GradientText(int16_t x, int16_t y, const char *text,
                      uint32_t c1, uint32_t c2, bool clear, uint8_t textCase);
    void drawJPG(int16_t x, int16_t y, const uint8_t *jpegData, uint32_t dataSize);
    void drawBMP(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h);
    void drawRGBBitmap(int16_t x, int16_t y, const uint32_t *bitmap, int16_t w, int16_t h);
    void drawBarChart(int16_t x, int16_t y, const int *data, uint8_t size,
                      bool withIcon, uint32_t color, uint32_t barBG);
    void drawLineChart(int16_t x, int16_t y, const int *data, uint8_t size,
                       bool withIcon, uint32_t color);
    void drawMenuIndicator(int cur, int total, uint32_t color);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color);
    void drawFilledRect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color);
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t color);
    void drawPixel(int16_t x, int16_t y, uint32_t color);
    void drawCircle(int16_t x0, int16_t y0, int16_t r, uint32_t color);
    void fillCircle(int16_t x0, int16_t y0, int16_t r, uint32_t color);
    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint32_t color);

    /* drawInstructions: small JSON DSL ([{"db":[x,y,w,h,color]}, ...]) */
    void processDrawInstructions(int16_t x, int16_t y, const char *drawInstructions);

    /* Pre-compiled drawInstructions execution path (consumed by the 60 Hz
     * custom-app render). The POD DrawOp vector is built once in
     * parseCustomPage so we do not re-parse JSON every frame. */
    void processCompiledDraw(int16_t x, int16_t y,
                              const std::vector<DrawOp> &ops,
                              const std::vector<std::string> &texts);

    /* Compiles a drawInstructions JSON string into a POD DrawOp vector.
     * Used by parseCustomPage to populate CustomApp::compiledDraw. Returns
     * false on malformed JSON; on failure `outOps`/`outTexts` are emptied. */
    static bool compileDrawInstructions(const char *json,
                                         std::vector<DrawOp> &outOps,
                                         std::vector<std::string> &outTexts);

    /* low-level passthroughs (mirror Adafruit GFX façade for custom code) */
    void matrixPrint(const char *str);
    void matrixPrint(char c);
    void matrixPrint(double number, uint8_t digits);
    void setCursor(int16_t x, int16_t y);
    void setTextColor(uint32_t color);
    void resetTextColor();
    void clearMatrix();
    void clear();
    void show();

    /* matrix state */
    void setPower(bool on);
    void powerStateParse(const char *json);
    void setBrightness(int bri);
    void applyAllSettings();
    void setNewSettings(const char *json);
    void setMatrixLayout(int layout);
    void gammaCorrection();
    void setCustomAppColors(uint32_t color);
    bool setAutoTransition(bool active);
    void showSleepAnimation();
    void sendAppLoop();
    void checkNewYear();
    void startArtnet();

    /* JSON */
    std::string getAppsAsJson() const;
    std::string getSettings() const;
    std::string getStats() const;
    std::string ledsAsJson() const;
    std::string getEffectNames() const;
    std::string getTransitionNames() const;
    std::string getAppsWithIcon() const;

    /* indicator */
    bool indicatorParser(uint8_t ind, const char *json);
    void setIndicator1Color(uint32_t c);
    void setIndicator1State(bool on);
    void setIndicator2Color(uint32_t c);
    void setIndicator2State(bool on);
    void setIndicator3Color(uint32_t c);
    void setIndicator3State(bool on);
    bool moodlight(const char *json);

    /* custom page */
    bool parseCustomPage(const char *name, const char *json, bool preventSave);

    /* apps vector */
    void updateAppVector(const char *json);
    void reorderApps(const char *json);

    /* matrix off */
    bool isMatrixOff() const { return m_matrixOff; }
    void setGameActive(bool a) { m_gameActive = a; }

    MatrixDisplayUi *getUI() { return m_ui; }
    Matrix          *getMatrix() { return m_matrix; }

    /* Read-only access to the custom-app registry; consumed by the
     * file-scope renderCustomApp() function in DisplayManager.cpp. */
    const std::map<std::string, CustomApp> &peekCustomApps() const;

    /* Read-only access to the apps vector — used by the file-scope
     * renderCustomApp() to translate `currentApp` index into the per-slot
     * app name (replaces the old, never-written userData scheme that left
     * custom apps silently un-rendered). Caller must hold the data lock
     * for the duration of any read that follows the returned reference. */
    const std::vector<std::pair<std::string, AppCallback>> &peekApps() const { return m_apps; }

    /* Exposed for the file-scope NotifyOverlay lambda in DisplayManager.cpp.
     * Mirrors the original AWTRIX3 design where Overlays.cpp directly
     * accesses the global `notifications` vector. */
    std::vector<AwtrixNotification> m_notifications;

    /* ── Thread-safety primitives ───────────────────────────────
     * HTTP, MQTT, TCP and the main loop all mutate m_apps / m_customApps /
     * m_notifications / the moodlight state concurrently. A single portMUX
     * serialises every public mutator and the per-frame render path; the
     * critical sections are deliberately short (microseconds) so this is
     * cheaper than a FreeRTOS mutex and safe even if a future caller turns
     * out to be an ISR. */
    portMUX_TYPE m_dataLock = portMUX_INITIALIZER_UNLOCKED;

    struct Lock {
        portMUX_TYPE *m;
        explicit Lock(portMUX_TYPE *l) : m(l) { portENTER_CRITICAL(m); }
        ~Lock() { portEXIT_CRITICAL(m); }
        Lock(const Lock&) = delete;
        Lock &operator=(const Lock&) = delete;
    };

private:
    DisplayManager() = default;
    Matrix          *m_matrix = nullptr;
    MatrixDisplayUi *m_ui = nullptr;
    bool m_matrixOff = false;
    bool m_gameActive = false;
    bool m_appIsSwitching = false;

    /* apps */
    std::vector<std::pair<std::string, AppCallback>> m_apps;
    std::map<std::string, CustomApp> m_customApps;
    std::string m_currentCustomApp;
};

/* ── Forward app callbacks ──────────────────────────────────── */
void TimeApp(Matrix &, UiState &, int16_t x, int16_t y, GifPlayer *);
void DateApp(Matrix &, UiState &, int16_t x, int16_t y, GifPlayer *);
void TempApp(Matrix &, UiState &, int16_t x, int16_t y, GifPlayer *);
void HumApp(Matrix &, UiState &, int16_t x, int16_t y, GifPlayer *);
void BatApp(Matrix &, UiState &, int16_t x, int16_t y, GifPlayer *);
void WeatherApp(Matrix &, UiState &, int16_t x, int16_t y, GifPlayer *);

#endif /* __cplusplus */