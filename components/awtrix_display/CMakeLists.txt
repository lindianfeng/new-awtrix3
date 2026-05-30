idf_component_register(
        SRCS "led_matrix.c" "matrix_cpp.cpp" "awtrix_font.cpp" "GifPlayer.cpp"
        INCLUDE_DIRS "."
        PRIV_REQUIRES led_strip awtrix_hal esp_timer
)