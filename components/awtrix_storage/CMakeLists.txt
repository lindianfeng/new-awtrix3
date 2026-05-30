idf_component_register(
        SRCS "awtrix_storage.c"
        INCLUDE_DIRS "."
        PRIV_REQUIRES awtrix_hal spi_flash spiffs
)