# awtrix_assets — read-only static data tables extracted from awtrix_core.
#
# These translation units are pure const data (icon bitmaps + locale strings)
# plus a couple of trivial lookup functions. They have:
#   - zero dependencies (just <stdint.h> / <string.h>)
#   - zero runtime state
#   - no business logic — pulling them out of awtrix_core keeps the core
#     library focused on DisplayManager / MatrixDisplayUi / menu / effects
#     and means edits to the asset tables don't trigger a core rebuild.
#
# Contents:
#   - awtrix_icons.{c,h}      : 8x8 RGB565 built-in icon set (~73 KB once
#                               the full demo + future emoji set is in).
#   - awtrix_dictionary.{c,h} : Localized weekday / month short names for
#                               en/de/fr/es/it/zh/ja.
#
# Layer: leaf — depends on nothing in the project. Anything that needs an
# icon lookup or a localized day/month name just adds awtrix_assets to its
# PRIV_REQUIRES (or REQUIRES if the symbol leaks through a public header).
idf_component_register(
        SRCS "awtrix_icons.c"
             "awtrix_dictionary.c"
        INCLUDE_DIRS "."
)
