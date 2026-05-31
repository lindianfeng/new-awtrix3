idf_component_register(
        SRCS "MatrixDisplayUi.cpp"
        "DisplayManager.cpp"
        "DisplayManager_customApp.cpp"
        "awtrix_functions.cpp"
        "awtrix_menumanager.cpp"
        INCLUDE_DIRS "."
        REQUIRES awtrix_kernel awtrix_display
        PRIV_REQUIRES awtrix_hal awtrix_config awtrix_assets cjson esp_timer
)
