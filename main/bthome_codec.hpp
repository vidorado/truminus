#pragma once
#include <cstdint>

// Pure BTHome v2 service-data parser (no ESP-IDF deps) so it can be host-tested
// against the real code — see test/host/test_bthome_parser.cpp.

// Walk an unencrypted BTHome v2 payload (the bytes after the device-info byte),
// looking for tag 0x2F (moisture uint8 → tank level) and tag 0x00 (packet ID).
// Returns true if a moisture sample was found, updating *pct; *seq is updated
// when a packet-ID tag is present. Unknown tags abort the walk (their length is
// unknown, so skipping would risk mis-parsing). See https://bthome.io/format/.
bool parseBthomePayload(const uint8_t* p, int len, uint8_t* pct, uint8_t* seq);
