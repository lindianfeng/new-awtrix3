# awtrix_kernel — cross-cutting contracts shared by all higher layers.
#
# Bundles seven previously-separate micro-components whose only role was to
# carry a struct / enum / registry / parser across the layer boundary:
#
#   - awtrix_events       (header-only event bus, breaks core<->periphery cycle)
#   - awtrix_commands     (AwtrixCommand enum + FreeRTOS-backed command queue)
#   - awtrix_apps         (AwtrixAppRegistry singleton: builtin + custom apps)
#   - awtrix_state        (Display + query JSON snapshots consumed by HTTP/MQTT)
#   - awtrix_protocol     (HTTP/MQTT payload -> AwtrixCommand parser, round 2)
#   - awtrix_placeholders (P1-D: MQTT topic → latest-value cache for
#                          customApp {{topic}} substitution)
#   - awtrix_system_state (L1: explicit device state machine — Boot/AP/
#                          Connecting/Normal/Moodlight/ArtNet/PoweredOff/
#                          EnteringSleep/Error)
#
# All seven are pure data/registry/parser code with no business logic. Keeping
# them in one leaf component eliminates 7 CMakeLists files, 7 sets of
# PRIV_REQUIRES bookkeeping, and the temptation to introduce more "shim"
# components just to break include cycles.
#
# Deps:
#   - awtrix_hal : portMUX_TYPE / MATRIX_WIDTH / MATRIX_HEIGHT / TAG_* macros
#   - cjson      : JSON parsing in awtrix_protocol + future snapshot serialization
#   - freertos / esp_timer / log are transitively provided by IDF core.
idf_component_register(
        SRCS "awtrix_command_bus.cpp"
             "awtrix_apps.cpp"
             "awtrix_display_snapshot.cpp"
             "awtrix_query_snapshot.cpp"
             "awtrix_protocol.cpp"
             "awtrix_placeholders.cpp"
             "awtrix_system_state.cpp"
        INCLUDE_DIRS "."
        REQUIRES awtrix_hal cjson
)
