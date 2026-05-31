#include "awtrix_render.h"

namespace
{
    constexpr uint32_t mode_bit(AwtrixDisplayMode mode)
    {
        return 1u << static_cast<uint8_t>(mode);
    }

    constexpr uint32_t kNormalModes = mode_bit(AwtrixDisplayMode::Boot) |
        mode_bit(AwtrixDisplayMode::Normal) |
        mode_bit(AwtrixDisplayMode::Notification) |
        mode_bit(AwtrixDisplayMode::Menu);

    constexpr AwtrixRenderLayerInfo kLayers[] = {
        {AwtrixRenderLayer::Background, "Background", 10, kNormalModes},
        {AwtrixRenderLayer::App, "App", 20, kNormalModes},
        {
            AwtrixRenderLayer::Notification, "Notification", 40,
            mode_bit(AwtrixDisplayMode::Notification) | mode_bit(AwtrixDisplayMode::Normal)
        },
        {AwtrixRenderLayer::WeatherOverlay, "WeatherOverlay", 50, kNormalModes},
        {AwtrixRenderLayer::Menu, "Menu", 60, mode_bit(AwtrixDisplayMode::Menu)},
        {AwtrixRenderLayer::Indicators, "Indicators", 70, kNormalModes | mode_bit(AwtrixDisplayMode::Moodlight)},
        {
            AwtrixRenderLayer::SystemStatus, "SystemStatus", 80,
            kNormalModes | mode_bit(AwtrixDisplayMode::Moodlight) | mode_bit(AwtrixDisplayMode::ArtNet)
        },
    };
}

AwtrixRenderEngine& AwtrixRenderEngine::get()
{
    static AwtrixRenderEngine instance;
    return instance;
}

const char* AwtrixRenderEngine::modeName() const
{
    switch (m_mode)
    {
    case AwtrixDisplayMode::Boot: return "Boot";
    case AwtrixDisplayMode::Normal: return "Normal";
    case AwtrixDisplayMode::Notification: return "Notification";
    case AwtrixDisplayMode::Menu: return "Menu";
    case AwtrixDisplayMode::Moodlight: return "Moodlight";
    case AwtrixDisplayMode::ArtNet: return "ArtNet";
    case AwtrixDisplayMode::Sleep: return "Sleep";
    case AwtrixDisplayMode::Off: return "Off";
    case AwtrixDisplayMode::Error: return "Error";
    }
    return "Unknown";
}

size_t AwtrixRenderEngine::currentVisibleLayers(const AwtrixRenderLayerInfo** out, size_t maxCount) const
{
    if (!out || maxCount == 0) return 0;
    const uint32_t bit = mode_bit(m_mode);
    size_t written = 0;
    for (const auto& info : kLayers)
    {
        if ((info.modeMask & bit) == 0) continue;
        if (written >= maxCount) break;
        out[written++] = &info;
    }
    return written;
}
