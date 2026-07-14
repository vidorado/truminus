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

    // ── OpenAir PLUS A/C (cooling) — only meaningful when the A/C is
    //    configured; the CLIMATIZACIÓN panel replaces CALEFACCIÓN then.
    //    Heat is still the Truma (heatingOn above); these drive cooling only.
    int   acMode;        // 0=off, 1=cool, 2=eco  (heat = heatingOn)
    bool  acFanAuto;     // cool mode: true=Auto (Mode AUTO), false=Man (Mode MAN)
    int   acFanSpeed;    // cool+Man blower speed, 1..6
    int   acFlap1;       // louver 1 mode — raw wire value 0/1 (see OA_FLAP_* in openairble.hpp)
    int   acFlap2;       // louver 2 mode — raw wire value 0/1
};
