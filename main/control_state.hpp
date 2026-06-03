#pragma once

// Plain data structs shared between the display, the LIN scheduler and the
// pure-logic modules. This header MUST stay free of ESP-IDF / LVGL / FreeRTOS
// includes so the logic that depends on it can be compiled and unit-tested on
// the host (see test/host/).

// ── Control state — reflects user button presses ─────────────────────────
struct P4ControlState {
    bool  heatingOn;
    int   fanMode;       // 0=off, 1=eco, 2=high, 3..12 = level 1..10
    int   boilerMode;    // 0=off, 1=40°C, 2=60°C, 3=boost(60°C)
    int   energyIdx;
    float roomSetpoint;
};
