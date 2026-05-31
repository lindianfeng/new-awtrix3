# awtrix_config — leaf component owning the AwtrixConfig singleton (the
# 70+ field persisted runtime config). Promoted out of awtrix_core so the
# periphery / network / http layers can read CONFIG.* without having to
# pull in DisplayManager.h (which used to create the
# awtrix_core ↔ awtrix_periphery cycle).
#
# Deps:
#   - awtrix_hal       : TAG_SYSTEM / pin macros + awtrix_settings_load_* /
#                        save_* NVS wrappers (former awtrix_storage, merged
#                        into hal in the first consolidation round) + CRGB
#                        type via awtrix_color.h (extracted from matrix_cpp.h
#                        in round 3 to break the spurious config→display
#                        public dependency).
#   - cjson            : AwtrixConfig::toJson() / save() format
#
# This component MUST stay leaf-ish: it can only depend on lower layers.
# Anything depending back on awtrix_core or any other higher layer would
# reintroduce a cycle.
idf_component_register(
        SRCS "awtrix_globals.cpp"
        INCLUDE_DIRS "."
        PRIV_REQUIRES awtrix_hal cjson
)
