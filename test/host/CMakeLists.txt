cmake_minimum_required(VERSION 3.14)
project(truminus_tests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Real production logic compiled straight from main/ (IDF-free modules only) so
# the tests exercise the shipping code, not a copy. test_helpers.cpp holds the
# remaining duplicated logic still pending the same treatment.
add_library(truminus_logic STATIC
    test_helpers.cpp
    catch_amalgamated.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../main/mode_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../main/version_compare.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../main/lin_codec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../main/bthome_codec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../main/am2301_codec.cpp
)
target_include_directories(truminus_logic PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../main
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
)

target_link_libraries(tests PRIVATE truminus_logic)

# Enable CTest
include(CTest)
add_test(NAME all_tests COMMAND tests)
