#include "test_helpers.hpp"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <endian.h>

// Fan/boiler conversions now live in main/mode_controller.cpp (linked into the
// test build) — no duplicate implementation here anymore.

// LIN value codec now lives in main/lin_codec.cpp (linked into the test build).

bool parseBthomePayload(const uint8_t* p, int len, uint8_t* pct, uint8_t* seq) {
    bool gotMoisture = false;
    int i = 0;
    while (i < len) {
        uint8_t tag = p[i++];
        int size;
        switch (tag) {
            case 0x00: size = 1; break;
            case 0x01: size = 1; break;
            case 0x02: size = 2; break;
            case 0x03: size = 2; break;
            case 0x12: size = 2; break;
            case 0x14: size = 2; break;
            case 0x2E: size = 1; break;
            case 0x2F: size = 1; break;
            case 0x4A: size = 2; break;
            case 0x51: size = 1; break;
            default:
                return gotMoisture;
        }
        if (i + size > len) return gotMoisture;
        if (tag == 0x2F) {
            *pct = p[i];
            gotMoisture = true;
        } else if (tag == 0x00) {
            *seq = p[i];
        }
        i += size;
    }
    return gotMoisture;
}

// Semver helpers now live in main/version_compare.cpp (linked into the test
// build) — no duplicate implementation here anymore.
