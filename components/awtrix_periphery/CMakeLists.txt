idf_component_register(
        SRCS "awtrix_io.c" "awtrix_periphery.cpp"
        "awtrix_power.cpp"
        "awtrix_dfplayer.c"
        INCLUDE_DIRS "."
        REQUIRES button
        PRIV_REQUIRES awtrix_hal awtrix_display awtrix_config awtrix_events esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_driver_uart esp_adc cjson esp_timer
)