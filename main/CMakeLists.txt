idf_component_register(
        SRCS "app_main.cpp"
        INCLUDE_DIRS "."
        PRIV_REQUIRES awtrix_hal awtrix_display awtrix_config awtrix_kernel awtrix_core awtrix_network awtrix_http awtrix_periphery esp_driver_gpio
)