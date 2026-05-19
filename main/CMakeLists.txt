# Embed C6 slave firmware if it has been built (run 'make build-c6' first).
# When present the binary is streamed to the C6 co-processor at startup via
# SDIO OTA if the version does not match the host ESP-Hosted library.
set(C6_FW_BIN "${CMAKE_CURRENT_SOURCE_DIR}/slave_fw/network_adapter.bin")
if(EXISTS "${C6_FW_BIN}")
    set(C6_FW_EMBED "slave_fw/network_adapter.bin")
    message(STATUS "TruMinus: C6 slave firmware found — will be embedded")
else()
    set(C6_FW_EMBED "")
    message(STATUS "TruMinus: C6 slave firmware absent — run 'make build-c6' then rebuild")
endif()

idf_component_register(
    SRCS
        "main.cpp"
        "p4display.cpp"
        "p4settings.cpp"
        "i18n.cpp"
        "lin_driver.cpp"
        "wifi_manager.cpp"
        "victronble.cpp"
        "ultimatronble.cpp"
        "c6_ota.cpp"
    INCLUDE_DIRS
        "."
    EMBED_FILES
        "font_regular.ttf"
        "font_bold.ttf"
        "font_icons.ttf"
        ${C6_FW_EMBED}
    REQUIRES
        jc4880_bsp
        nvs_flash
        esp_event
        esp_netif
        esp_http_server
        esp_driver_uart
        esp_driver_gpio
        esp_timer
        esp_wifi
        mbedtls
        freertos
        h2zero__esp-nimble-cpp
        espressif__esp_hosted
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    "-include" "${CMAKE_CURRENT_SOURCE_DIR}/dummy_flags.h"
)

if(EXISTS "${C6_FW_BIN}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE C6_FW_AVAILABLE=1)
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE ENABLE_BLE=1)
