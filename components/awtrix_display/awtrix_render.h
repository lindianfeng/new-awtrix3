#pragma once

#ifdef __cplusplus

#include <stddef.h>
#include <stdint.h>

enum class AwtrixDisplayMode : uint8_t
{
    Boot,
    Normal,
    Notification,
    Menu,
    Moodlight,
    ArtNet,
    Sleep,
    Off,
    Error,
};

enum class AwtrixRenderLayer : uint8_t
{
    Background,
    App,
    Notification,
    WeatherOverlay,
    Menu,
    Indicators,
    SystemStatus,
};

struct AwtrixRenderLayerInfo
{
    AwtrixRenderLayer layer;
    const char* name;
    uint8_t priority;
    uint32_t modeMask;
};

struct AwtrixRenderContext
{
    uint32_t nowMs = 0;
    AwtrixDisplayMode mode = AwtrixDisplayMode::Normal;
};

class AwtrixRenderEngine
{
public:
    static AwtrixRenderEngine& get();

    void setMode(AwtrixDisplayMode mode) { m_mode = mode; }
    AwtrixDisplayMode mode() const { return m_mode; }
    const char* modeName() const;

    /* Currently the only consumer is MatrixDisplayUi::update(), which walks
     * the per-frame visible-layer list to decide what to draw. The previous
     * generic queries (layers(), layerVisibleInMode(), visibleLayersForMode())
     * were never called and were removed in round 5. */
    size_t currentVisibleLayers(const AwtrixRenderLayerInfo** out, size_t maxCount) const;

private:
    AwtrixRenderEngine() = default;
    AwtrixDisplayMode m_mode = AwtrixDisplayMode::Normal;
};

#define RENDER_ENGINE (AwtrixRenderEngine::get())

#endif /* __cplusplus */
