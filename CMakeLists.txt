# TruMinus — top-level CMakeLists.txt (ESP-IDF native)
#
# Setup once:   ~/esp/esp-idf/install.sh esp32p4
# Each shell:   . ~/esp/esp-idf/export.sh
# Build:        idf.py set-target esp32p4 && idf.py build
# Flash:        idf.py -p /dev/ttyACM0 flash monitor
# Target chip:  ESP32-P4 ECO2 (rev 1.x) on JC4880P443C-I-W-Y board

cmake_minimum_required(VERSION 3.16)

# Pre-build: rewrite cache-bust query strings in data/index.html so the
# LittleFS image picks up the new hash.  Web assets themselves are no
# longer embedded — the LittleFS image is built by main/CMakeLists.txt
# via littlefs_create_partition_image() and flashed from <project>/data/.
#
# NOTE: apply_patches.py is NOT here — it has to run AFTER project() because
# the Component Manager only resolves and downloads managed_components during
# the project() call.  Running it before would no-op (target files don't exist
# yet) and patches would never apply.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    execute_process(
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/cache_bust.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Project-wide compile definitions — set BEFORE project() so they are visible
# to all components during idf_build_process.  One call per definition because
# idf_build_set_property accepts a single value argument.
#   NO_MQTT     — MQTT subsystem not yet ported.
#   WEBSERVER   — enables the embedded HTTP/WebSocket server.
#   JC4880_P4   — board identifier for conditional code paths.
idf_build_set_property(COMPILE_DEFINITIONS "NO_MQTT"           APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "WEBSERVER"         APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "JC4880_P4"         APPEND)

project(truminus)

# Patch managed_components AFTER project() has resolved + downloaded them.
# Each entry in apply_patches.py is idempotent (no-op on second run), so
# re-running on every configure is safe.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    execute_process(
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/apply_patches.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()
