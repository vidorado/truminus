# TruMinus — top-level CMakeLists.txt (ESP-IDF native)
#
# Setup once:   ~/esp/esp-idf/install.sh esp32p4
# Each shell:   . ~/esp/esp-idf/export.sh
# Build:        idf.py set-target esp32p4 && idf.py build
# Flash:        idf.py -p /dev/ttyACM0 flash monitor
# Target chip:  ESP32-P4 ECO2 (rev 1.x) on JC4880P443C-I-W-Y board

cmake_minimum_required(VERSION 3.16)

# Web-asset preparation now runs at BUILD time via a custom target (see
# below, after project()).  Doing it at configure time only — as we did
# before — meant `idf.py build` after editing data/ would build a stale
# LittleFS image: the script ran during the previous reconfigure, not
# during the build itself.  Both cache_bust.py and gen_gz.py are
# idempotent, so re-running on every build is cheap.
#
# NOTE: apply_patches.py is NOT here — it has to run AFTER project() because
# the Component Manager only resolves and downloads managed_components during
# the project() call.  Running it before would no-op (target files don't exist
# yet) and patches would never apply.

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

# Firmware version — embedded into esp_app_desc_t.version and used by the
# self-OTA path (main/p4_ota.cpp) to compare against the latest GitHub
# release tag.  Derived from `git describe`: exactly "X.Y.Z" when sitting on
# a tag, "X.Y.Z-<n>-g<sha>[-dirty]" otherwise.  Falls back to "0.0.0" when
# git is unavailable (e.g. building from a tarball).  Must be set before
# project() for IDF to pick it up; max 31 chars fit in the app descriptor.
#
# git describe runs at *configure* time, and cmake won't reconfigure just
# because the git state changed — so an incremental build/flash after a new
# commit or tag would keep baking the stale version (we shipped 1.1.0-dirty
# this way once).  Watch the reflog + packed-refs as configure-dependencies
# so a commit/checkout forces a reconfigure and re-runs git describe.  This
# is *reconfigure only* (re-parse + regen ninja, a few seconds) — ninja still
# builds incrementally, and only the app-descriptor object that embeds
# PROJECT_VER is recompiled.  We deliberately do NOT watch .git/index: it
# changes on every `git add`, which would tax the dev loop for no benefit
# (the version only changes on commit/checkout, captured by logs/HEAD; a
# bare `git tag` is picked up by the reconfigure already pending from the
# commit that precedes it).
foreach(_gitfile logs/HEAD packed-refs)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/${_gitfile}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${CMAKE_CURRENT_SOURCE_DIR}/.git/${_gitfile}")
    endif()
endforeach()
execute_process(
    COMMAND git describe --tags --dirty --always
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE TRUMINUS_GIT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(TRUMINUS_GIT_VER)
    set(PROJECT_VER "${TRUMINUS_GIT_VER}")
else()
    set(PROJECT_VER "0.0.0")
endif()
message(STATUS "TruMinus: firmware version = ${PROJECT_VER}")

project(truminus)

# Append --skip-flashed to every esptool write-flash invocation.  esptool
# computes the MD5 of each image and compares against on-flash content;
# unchanged images are skipped.  This is mainly worth it for the LittleFS
# partition: joltwallet/littlefs's helper regenerates littlefs.bin on
# every cmake configure (it has no way to track data/ contents), so
# without this flag a 25 s rewrite happens on every `idf.py flash`.
# littlefs-python is deterministic — same data/ → identical bin → skipped.
#
# We mutate the SUB_ARGS property on each existing flash target rather than
# the component-level FLASH_SUB_ARGS, because by the time project() returns
# esptool_py has already called esptool_py_flash_target() for each target
# and frozen FLASH_SUB_ARGS into the target's SUB_ARGS property.
foreach(t flash app-flash partition-table-flash bootloader-flash littlefs-flash)
    if(TARGET ${t})
        get_target_property(_subargs ${t} SUB_ARGS)
        set_target_properties(${t} PROPERTIES SUB_ARGS "${_subargs} --skip-flashed")
    endif()
endforeach()

# Web-asset preparation target — runs on every build before the LittleFS
# image is regenerated.  cache_bust.py rewrites ?v=<hash> querystrings in
# data/index.html (and url(...) refs inside CSS) so browsers refetch any
# changed asset.  gen_gz.py pre-compresses HTML/CSS/JS/TTF into adjacent
# .gz files; the server prefers the .gz variant and sets Content-Encoding.
#
# Both scripts are idempotent: when nothing changed they print "up to
# date" and return immediately, so making them ALL targets is cheap.
# IDF passes the interpreter as -DPYTHON=…; PYTHON_EXECUTABLE is not set in
# the CI container (only some local setups define it), so an empty prefix made
# cmake try to exec the scripts directly → "Permission denied".  Use the
# IDF-provided ${PYTHON}, falling back to PYTHON_EXECUTABLE / "python3".
if(PYTHON)
    set(_TRUMINUS_PY "${PYTHON}")
elseif(PYTHON_EXECUTABLE)
    set(_TRUMINUS_PY "${PYTHON_EXECUTABLE}")
else()
    set(_TRUMINUS_PY "python3")
endif()
add_custom_target(web_assets_prep ALL
    COMMAND ${_TRUMINUS_PY} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/cache_bust.py
    COMMAND ${_TRUMINUS_PY} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_gz.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Refreshing web asset cache-bust hashes + .gz variants"
)
if(TARGET littlefs_littlefs_bin)
    add_dependencies(littlefs_littlefs_bin web_assets_prep)
endif()

# Patch managed_components AFTER project() has resolved + downloaded them.
# Each entry in apply_patches.py is idempotent (no-op on second run), so
# re-running on every configure is safe.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    execute_process(
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/apply_patches.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()
