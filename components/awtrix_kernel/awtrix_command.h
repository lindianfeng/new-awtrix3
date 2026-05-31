#pragma once

#ifdef __cplusplus

#include <stdint.h>
#include <string>

enum class AwtrixCommandType : uint8_t
{
    Notify,
    DismissNotify,
    CustomApp,
    Settings,
    UpdateApps,
    SwitchApp,
    NextApp,
    PreviousApp,
    Power,
    Sleep,
    Moodlight,
    ReorderApps,
    Indicator,
    Button,
    SetBrightness,
    ShowSleepScreen,
    Rtttl,
    Sound,
    R2D2,
    SetVolume,
    Reboot,
    FactoryReset,
};

enum AwtrixCommandSource : uint8_t
{
    AWTRIX_COMMAND_SOURCE_INTERNAL = 0,
    AWTRIX_COMMAND_SOURCE_HTTP = 1,
    AWTRIX_COMMAND_SOURCE_MQTT = 2,
    AWTRIX_COMMAND_SOURCE_BUTTON = 3,
    AWTRIX_COMMAND_SOURCE_PERIPHERY = 4,
    AWTRIX_COMMAND_SOURCE_SYSTEM = 5,
};

struct AwtrixCommand
{
    AwtrixCommandType type;
    uint8_t source = AWTRIX_COMMAND_SOURCE_INTERNAL;
    uint8_t index = 0;
    std::string name;
    std::string payload;
};

#endif /* __cplusplus */
