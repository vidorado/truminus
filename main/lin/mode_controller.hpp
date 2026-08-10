#pragma once

#include <cstdint>
#include "control_state.hpp"

// Fan mode conversions
// 0=off, 1=eco, 2=high, 3..12 = level 1..10
int fanStrToInt(const char* v);
const char* fanIntToStr(int m);

// Boiler mode conversions
// 0=off, 1=eco, 2=high, 3=boost
int boilerStrToInt(const char* v);
const char* boilerIntToStr(int m);

struct TrumaLinSetpoints {
    uint8_t pumpOrFan;
    double roomSp;
    double waterSp;
};

/**
 * Derive the LIN setpoints from the current control state.
 *
 * Translates the UI state (heating/fan/boiler) into the values the Truma LIN
 * protocol expects:
 * - pumpOrFan: fan/pump mode (0x10-0x1A for fan-only, 1-2 while heating)
 * - roomSp:    target room temperature in °C (0 when heating is off)
 * - waterSp:   target water temperature in °C (40/60 depending on boilerMode)
 */
TrumaLinSetpoints derive_mode(const P4ControlState& cs);
