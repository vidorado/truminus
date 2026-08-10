#pragma once
#include <cstdint>

// Pure Truma LIN value codec: Kelvin×10 temperature encode/decode and the
// frame-0x21/0x22 field parsers. No ESP-IDF deps, so it is host-testable
// against the real code — see test/host/test_lin_encoding.cpp.
//
// The frame *assembly* helpers (f20_set*, f05_set*, …) stay in truma_lin.cpp
// because they mutate the shared frame buffers; only the stateless value
// conversions live here.

// Encode `celsius` as little-endian Kelvin×10 into dest[0..1].
void encodeTempKelvinX10(double celsius, uint8_t* dest /*2 bytes*/);

// Frame 0x21 temperature parsers. Range gates: room ∈ [0,50] °C,
// water ∈ [0,100] °C — anything outside returns NAN.
double parseF21RoomTemp(const uint8_t* d);
double parseF21WaterTemp(const uint8_t* d);

// Frame 0x22 byte 1: 0x40/0x50 = water heating active.
bool parseF22WaterHeating(const uint8_t* d);

// True when `c` is an error class the Truma actually defines (see the
// truma-protocol skill). GATE EVERY FAULT SURFACE ON THIS: the 0x3D read can
// arrive one byte misaligned and echo the request back as class 0x46 / code
// 0x20, which would otherwise be reported as a real fault.
bool trumaClassKnown(uint8_t c);
