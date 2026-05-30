# Header-only component — INTERFACE_LIBRARY equivalent in IDF.
# Provides awtrix_events.h, an std::function event bus that decouples
# awtrix_core from awtrix_periphery. No source files; we list a 1-line
# stub source so idf_component_register doesn't reject the empty SRCS.
idf_component_register(
        INCLUDE_DIRS "."
)
