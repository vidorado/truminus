#pragma once
#include <cstdint>
#include <cstddef>

// Pure AM2301/DHT22 symbol decoder (no ESP-IDF deps) so it can be host-tested
// against the real code — see test/host/test_am2301_decode.cpp. am2301.cpp
// captures rmt_symbol_word_t via RMT, copies them into Am2301Symbol (a trivial
// field copy) and calls am2301Decode().

#define AM2301_MAX_SYMBOLS     48   // RX buffer depth / max bits collected
#define AM2301_BIT_THRESH_US   50   // high-pulse length above this = bit 1

// Minimal mirror of the relevant rmt_symbol_word_t fields.
struct Am2301Symbol {
    uint16_t duration0;
    uint8_t  level0;
    uint16_t duration1;
    uint8_t  level1;
};

// Decode the captured symbol stream into the 5 raw bytes; fills tempC/humidity
// and returns true on a valid checksum. Over-collects high pulses and keeps the
// last 40 data bits, sidestepping the sensor's leading handshake.
bool am2301Decode(const Am2301Symbol* syms, size_t n, float& tempC, float& humidity);
