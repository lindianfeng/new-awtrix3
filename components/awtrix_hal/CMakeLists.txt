idf_component_register(
        SRCS "awtrix_hal.c" "awtrix_utils.c"
        PRIV_REQUIRES nvs_flash
        INCLUDE_DIRS "."
)
