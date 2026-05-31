idf_component_register(
        SRCS "awtrix_io.c" "awtrix_periphery.cpp"
        "awtrix_power.cpp"
        "awtrix_dfplayer.c"
        INCLUDE_DIRS "."
        # Round 3 header hygiene: awtrix_periphery.h no longer exposes
        # iot_button.h / button_gpio.h, so `button` can live in
        # PRIV_REQUIRES instead of leaking into every downstream component.
        PRIV_REQUIRES awtrix_hal awtrix_config awtrix_kernel button esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_driver_uart esp_adc cjson esp_timer
)