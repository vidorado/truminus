#pragma once
#include <cstdint>
#include "ultimatronble.hpp"   // UltimatronData (IDF-free)

// Pure Ultimatron BMS GATT response parser (no ESP-IDF / NimBLE deps) so it can
// be host-tested against the real code — see test/host/test_ultimatron_parser.cpp.
// The GATT plumbing stays in ultimatronble.cpp.

// Parse a 0xDD 0x03 basic-info response. Returns false (leaving `out` untouched)
// if the packet is too short or the header bytes don't match. On success fills
// out.battV/battA/soc/tempC/valid (tempC only when len >= 30, else 0) and leaves
// out.lastMs for the caller to timestamp.
bool ultimatronParseResponse(const uint8_t* d, int len, UltimatronData& out);
