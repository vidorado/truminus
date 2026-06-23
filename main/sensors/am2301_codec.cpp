#include "am2301_codec.hpp"

bool am2301Decode(const Am2301Symbol* syms, size_t n, float& tempC, float& humidity) {
    // Each data bit is a low (~50 µs) followed by a high whose length
    // distinguishes 0 from 1. We collect every high-pulse duration; the stream
    // carries a couple of leading highs (host release + sensor 80 µs handshake)
    // before the 40 data bits, so we over-collect and keep the last 40 below.
    uint8_t bits[AM2301_MAX_SYMBOLS];
    int     nbits = 0;

    for (size_t i = 0; i < n && nbits < (int)sizeof(bits); i++) {
        uint16_t hi_us = 0;
        if (syms[i].level0 == 1)       hi_us = syms[i].duration0;
        else if (syms[i].level1 == 1)  hi_us = syms[i].duration1;
        else                           continue;          // low-low: skip
        if (hi_us == 0) continue;
        bits[nbits++] = (hi_us > AM2301_BIT_THRESH_US) ? 1 : 0;
    }

    if (nbits < 40) return false;
    // Keep the last 40 bits (skip any leading handshake high captured as a
    // separate symbol).
    const uint8_t* b = bits + (nbits - 40);

    uint8_t raw[5] = {0};
    for (int i = 0; i < 40; i++)
        raw[i / 8] = (raw[i / 8] << 1) | b[i];

    uint8_t sum = raw[0] + raw[1] + raw[2] + raw[3];
    if (sum != raw[4]) return false;

    humidity = ((raw[0] << 8) | raw[1]) / 10.0f;
    int16_t t = ((raw[2] & 0x7F) << 8) | raw[3];
    tempC = (raw[2] & 0x80) ? -t / 10.0f : t / 10.0f;
    return true;
}
