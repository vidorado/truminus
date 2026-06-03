#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>

// Fan/boiler conversions moved to the real production module: see
// main/mode_controller.{hpp,cpp}, exercised by test_fan_boiler.cpp.

// LIN value codec (encodeTempKelvinX10, parseF21*, parseF22*) moved to the real
// production module: see main/lin_codec.{hpp,cpp}, exercised by
// test_lin_encoding.cpp.

// BTHome parser moved to the real production module: see
// main/bthome_codec.{hpp,cpp}, exercised by test_bthome_parser.cpp.

// Semver helpers moved to the real production module: see
// main/version_compare.{hpp,cpp}, exercised by test_semver.cpp.
