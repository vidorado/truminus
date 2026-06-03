#include "lin_codec.hpp"
#include <cstring>
#include <cmath>
#include <endian.h>

void encodeTempKelvinX10(double celsius, uint8_t* dest /*2 bytes*/) {
    uint16_t raw = (uint16_t)htole16((uint16_t)lround((celsius + 273.0) * 10.0));
    memcpy(dest, &raw, 2);
}

// byte 0   = Kelvin×10 LSB (bits 7:0 of room temp 12-bit value)
// byte 1   = bits 3:0 → room K×10 bits 11:8, bits 7:4 → water K×10 bits 3:0
// byte 2   = water K×10 bits 11:4
double parseF21RoomTemp(const uint8_t* d) {
    uint16_t raw = (uint16_t)d[0] | ((uint16_t)(d[1] & 0x0F) << 8);
    double t = raw / 10.0 - 273.0;
    return (t < 0.0 || t > 50.0) ? NAN : t;
}

double parseF21WaterTemp(const uint8_t* d) {
    uint16_t raw = (uint16_t)(d[1] >> 4) | ((uint16_t)d[2] << 4);
    double t = raw / 10.0 - 273.0;
    return (t < 0.0 || t > 100.0) ? NAN : t;
}

bool parseF22WaterHeating(const uint8_t* d) {
    return (d[1] & 0xC0) == 0x40;
}
