idf_component_register(
        SRCS "MatrixDisplayUi.cpp"
        "DisplayManager.cpp"
        "effects_core.cpp"
        "awtrix_functions.cpp"
        "awtrix_dictionary.c"
        "awtrix_icons.c"
        "awtrix_menumanager.cpp"
        "awtrix_games.cpp"
        INCLUDE_DIRS "."
        PRIV_REQUIRES awtrix_display awtrix_hal awtrix_storage awtrix_config awtrix_events cjson esp_timer
)