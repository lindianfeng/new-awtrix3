idf_component_register(
        SRCS "awtrix_network.cpp"
        "awtrix_mqtt.cpp"
        "awtrix_artnet.cpp"
        "awtrix_tcp_game.cpp"
        INCLUDE_DIRS "."
        REQUIRES esp_wifi esp_netif mdns lwip espressif__mqtt esp_http_client esp_event mbedtls esp-tls espressif__cjson
        PRIV_REQUIRES awtrix_hal awtrix_core awtrix_storage awtrix_config awtrix_periphery awtrix_display
)