idf_component_register(
        SRCS "awtrix_hal.c"
        "awtrix_utils.c"
        "awtrix_storage.c"
        PRIV_REQUIRES nvs_flash spi_flash spiffs
        INCLUDE_DIRS "."
)
