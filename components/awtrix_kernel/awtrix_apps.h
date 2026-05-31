#pragma once

#ifdef __cplusplus

#include <stdint.h>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

class Matrix;
struct UiState;
class GifPlayer;
using AppCallback = std::function<void(Matrix &, UiState &, int16_t, int16_t, GifPlayer *)>;

/* ── Compiled drawInstructions opcode table ────────────────────── */
enum DrawOpCode : uint8_t
{
    OPC_NONE = 0,
    OPC_PIXEL,
    OPC_LINE,
    OPC_FILLRECT,
    OPC_RECT,
    OPC_FILLCIRCLE,
    OPC_CIRCLE,
    OPC_TEXT,
    OPC_PALETTE,
    OPC_PALETTE_PIX,
};

struct DrawOp
{
    uint8_t op;
    int16_t a[4];
    uint32_t color;
    uint8_t textIdx;
    uint8_t paletteSize;
    uint32_t palette[4];
};

/* ── Custom app data ───────────────────────────────────────── */
struct CustomApp
{
    std::string name;
    std::string text;
    std::string drawInstructions;
    std::vector<DrawOp> compiledDraw;
    std::vector<std::string> drawTexts;
    std::string iconName;
    uint32_t color = 0xFFFFFF;
    int effect = -1;
    int gradient[2] = {0, 0};
    bool rainbow = false;
    bool center = false;
    bool bounce = false;
    bool topText = true;
    bool noScrolling = true;
    int bounceDir = 0;
    int textCase = 0;
    int textOffset = 0;
    int iconOffset = 0;
    int fade = 0;
    int blink = 0;
    float scrollSpeed = 100;
    float scrollposition = 0;
    float iconPosition = 0;
    int16_t scrollDelay = 0;
    int repeat = -1;
    int16_t currentRepeat = 0;
    long duration = 0;
    long lastUpdate = 0;
    uint64_t lifetime = 0;
    uint8_t lifetimeMode = 0;
    bool lifeTimeEnd = false;
    uint8_t currentFrame = 0;
    bool hasCustomColor = false;
    bool isGif = false;
    /* Pack H: bar/line chart autoscale toggle (default true, matches the
     * original generateCustomPage default). When false, raw values are
     * clamped to [0..100] and proportionally mapped instead. */
    bool autoscale = true;
    /* Pack H: raw JSON forwarded to the effects engine (e.g. fx tuning
     * knobs). Stored as a string so we don't pull cJSON into this header. */
    std::string effectSettings;
    uint8_t pushIcon = 0;
    bool iconWasPushed = false;
    int barData[16] = {0};
    uint32_t barBG = 0;
    int barSize = 0;
    int lineData[16] = {0};
    int lineSize = 0;
    int progress = -1;
    uint32_t pColor = 0;
    uint32_t pbColor = 0;
    uint32_t background = 0;
    std::vector<uint32_t> colors;
    std::vector<std::string> fragments;
};

class AwtrixAppRegistry
{
public:
    static AwtrixAppRegistry& get();

    std::vector<std::pair<std::string, AppCallback>>& apps() { return m_apps; }
    const std::vector<std::pair<std::string, AppCallback>>& apps() const { return m_apps; }
    std::map<std::string, CustomApp>& customApps() { return m_customApps; }
    const std::map<std::string, CustomApp>& customApps() const { return m_customApps; }

    void clearApps();
    void addApp(const std::string& name, AppCallback callback);
    void replaceApps(std::vector<std::pair<std::string, AppCallback>> apps);
    void eraseApp(const std::string& name);
    int findAppIndex(const std::string& name) const;
    std::string appNameAt(size_t index) const;
    size_t appCount() const { return m_apps.size(); }
    bool appsEmpty() const { return m_apps.empty(); }
    void upsertCustomApp(const std::string& name, CustomApp app);
    void eraseCustomApp(const std::string& name);
    CustomApp* findCustomApp(const std::string& name);
    const CustomApp* findCustomApp(const std::string& name) const;

private:
    AwtrixAppRegistry() = default;
    std::vector<std::pair<std::string, AppCallback>> m_apps;
    std::map<std::string, CustomApp> m_customApps;
};

#define APP_REGISTRY (AwtrixAppRegistry::get())

#endif /* __cplusplus */
