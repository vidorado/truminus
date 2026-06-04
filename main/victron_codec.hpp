#pragma once
#include <cstdint>
#include "victronble.hpp"   // VictronData (IDF-free)

// Pure Victron SolarCharger Instant-Readout field decode (no ESP-IDF / mbedtls /
// NimBLE deps) so it can be host-tested against the real code — see
// test/host/test_victron_parser.cpp. The AES-CTR decryption and the NimBLE
// advert plumbing stay in victronble.cpp; this takes the 16-byte plaintext.

// Decode a decrypted SolarCharger record into `out`. Returns false (leaving
// `out` untouched) when the voltage field is the 0x7FFF "not available"
// sentinel. On success fills out.state/errCode/battV/battA/kWhToday/pvW and sets
// valid = true, leaving out.lastMs for the caller to timestamp.
bool victronParseRecord(const uint8_t* pt, VictronData& out);
