#pragma once
#include <cstdint>

// Pure Truma CP-Plus frame-field encoders (no ESP-IDF deps) so the bit-packing
// is host-tested against the real code — see test/host/test_lin_frames.cpp.
// Each writes into the caller's frame data buffer at the canonical byte offsets;
// truma_lin.cpp's f20_set*/f05_*/… wrappers pass the shared frame buffers.

// Frame 0x20 bytes 0-1: room setpoint as 12-bit K×10 + flags nibble 0xA.
// celsius outside [5,30] → 0xAA 0xAA (heating-off sentinel).
void linF20Room(double celsius, uint8_t* f20);

// Frame 0x20 byte 2: water setpoint as K×10 >> 4. celsius < 1 → 0xAA.
void linF20Water(double celsius, uint8_t* f20);

// Frame 0x20 byte 5: high nibble = heating/fan mode (1→0xB eco, 2→0xD high,
// >=0x10 → fan level nibble), low nibble = water mode.
void linF20FanWater(uint8_t pumpOrFan, uint8_t waterMode, uint8_t* f20);

// Frame 0x05 byte 0: energy-selection priority code for energyIdx 0..4.
void linF05Energy(int energyIdx, uint8_t* f05);

// Frame 0x06 bytes 0-1: electric power limit (little-endian watts) for idx 0..4.
void linF06PowerLimit(int energyIdx, uint8_t* f06);

// Frame 0x07 bytes 0-1: pump/fan request (pumpOrFan | 0xE0, then 0xFE).
void linF07PumpOrFan(uint8_t pumpOrFan, uint8_t* f07);
