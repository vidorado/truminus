cmake_minimum_required(VERSION 3.14)
project(truminus_tests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Real production logic compiled straight from main/ (IDF-free modules only) so
# the tests exercise the shipping code, not a copy. Every test now links the real
# module — no inline re-implementations remain.
#
# Paths mirror main/'s subdirectory layout, and the include list mirrors
# main/CMakeLists.txt::INCLUDE_DIRS so the modules resolve each other's headers
# exactly as they do in the firmware build. Moving a module between subdirs
# means updating both lists.
set(TRUMINUS_MAIN ${CMAKE_CURRENT_SOURCE_DIR}/../../main)

add_library(truminus_logic STATIC
    catch_amalgamated.cpp
    ${TRUMINUS_MAIN}/lin/mode_controller.cpp
    ${TRUMINUS_MAIN}/lin/lin_codec.cpp
    ${TRUMINUS_MAIN}/lin/lin_protocol.cpp
    ${TRUMINUS_MAIN}/lin/lin_frames.cpp
    ${TRUMINUS_MAIN}/web/ws_command.cpp
    ${TRUMINUS_MAIN}/web/ws_diff.cpp
    ${TRUMINUS_MAIN}/web/ws_frames.cpp
    ${TRUMINUS_MAIN}/ota/version_compare.cpp
    ${TRUMINUS_MAIN}/ble/bthome_codec.cpp
    ${TRUMINUS_MAIN}/ble/multiplus_codec.cpp
    ${TRUMINUS_MAIN}/ble/ultimatron_codec.cpp
    ${TRUMINUS_MAIN}/ble/victron_codec.cpp
    ${TRUMINUS_MAIN}/sensors/am2301_codec.cpp
    ${TRUMINUS_MAIN}/core/faultlog_codec.cpp
)
target_include_directories(truminus_logic PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${TRUMINUS_MAIN}
    ${TRUMINUS_MAIN}/app
    ${TRUMINUS_MAIN}/ui
    ${TRUMINUS_MAIN}/web
    ${TRUMINUS_MAIN}/lin
    ${TRUMINUS_MAIN}/ble
    ${TRUMINUS_MAIN}/sensors
    ${TRUMINUS_MAIN}/ota
    ${TRUMINUS_MAIN}/core
)

# Test executable
add_executable(tests
    test_fan_boiler.cpp
    test_lin_encoding.cpp
    test_bthome_parser.cpp
    test_multiplus_bitreader.cpp
    test_semver.cpp
    test_lin_frames.cpp
    test_derive_mode.cpp
    test_lin_protocol.cpp
    test_am2301_decode.cpp
    test_faultlog.cpp
    test_multiplus_parser.cpp
    test_ultimatron_parser.cpp
    test_victron_parser.cpp
    test_ws_command.cpp
    test_ws_diff.cpp
    test_ws_frames.cpp
)

target_link_libraries(tests PRIVATE truminus_logic)

# Enable CTest
include(CTest)
add_test(NAME all_tests COMMAND tests)
