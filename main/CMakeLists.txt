# Explicit source list — only files ported to ESP-IDF.
# Add here as each module is migrated from Arduino.
idf_component_register(
    SRCS
        "main.cpp"
        "p4display.cpp"
        "p4settings.cpp"
        "i18n.cpp"
        "lin_driver.cpp"
    INCLUDE_DIRS
        "."
    EMBED_FILES
        "font_regular.ttf"
        "font_bold.ttf"
        "font_icons.ttf"
    REQUIRES
        jc4880_bsp
        nvs_flash
        esp_event
        esp_netif
        esp_http_server
        esp_driver_uart
        esp_driver_gpio
        esp_timer
        freertos
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    "-include" "${CMAKE_CURRENT_SOURCE_DIR}/dummy_flags.h"
)
