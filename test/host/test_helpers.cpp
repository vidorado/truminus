#include "test_helpers.hpp"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <endian.h>

// Fan/boiler conversions now live in main/mode_controller.cpp (linked into the
// test build) — no duplicate implementation here anymore.

// LIN value codec now lives in main/lin_codec.cpp (linked into the test build).

// BTHome parser now lives in main/bthome_codec.cpp (linked into the test build).
// Semver helpers now live in main/version_compare.cpp (linked into the test
// build) — no duplicate implementation here anymore.
