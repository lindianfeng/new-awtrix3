idf_component_register(
        SRCS "awtrix_http.cpp" "awtrix_api.cpp"
        INCLUDE_DIRS "."
        REQUIRES esp_http_server
        PRIV_REQUIRES awtrix_hal awtrix_core awtrix_config awtrix_display awtrix_kernel awtrix_network awtrix_periphery spiffs esp_http_client
)