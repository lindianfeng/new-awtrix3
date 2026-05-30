cmake_minimum_required(VERSION 3.22)

# AWTRIX3 ESP-IDF port – Target: ESP32-S3 (Ulanzi clock)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(awtrix3)

set_source_files_properties(
        ${IDF_PATH}/components/esp_lcd/rgb/esp_lcd_panel_rgb.c
        PROPERTIES COMPILE_FLAGS "-O0"
)