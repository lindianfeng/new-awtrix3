# awtrix_config — leaf component owning the AwtrixConfig singleton (the
# 70+ field persisted runtime config). Promoted out of awtrix_core so the
# periphery / network / http layers can read CONFIG.* without having to
# pull in DisplayManager.h (which used to create the
# awtrix_core ↔ awtrix_periphery cycle).
#
# Deps:
#   - awtrix_hal       : TAG_SYSTEM / pin macros
#   - awtrix_storage   : awtrix_settings_load_* / save_* NVS wrappers
#   - awtrix_display   : CRGB type used by colorCorrection / colorTemperature
#   - cjson            : AwtrixConfig::toJson() / save() format
#
# This component MUST stay leaf-ish: it can only depend on lower layers.
# Anything depending back on awtrix_core or any other higher layer would
# reintroduce a cycle.
idf_component_register(
        SRCS "awtrix_globals.cpp"
        INCLUDE_DIRS "."
        PRIV_REQUIRES awtrix_hal awtrix_storage awtrix_display cjson
)
