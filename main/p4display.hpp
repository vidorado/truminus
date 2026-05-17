#pragma once
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include <stdint.h>

// ── Data structs fed to p4DisplayUpdate() ────────────────────────────────

struct P4SolarData {
    const char* status;   // "Bulk" / "Absorción" / "Float" / "Apagado" / ...
    float       voltageV;
    float       currentA;
    int         powerW;
};

struct P4BattData {
    int   soc;        // 0–100 %
    float voltageV;
};

struct P4DisplayData {
    // Temperatures — value < -100 means "invalid / no data"
    float roomTemp;
    float waterTemp;
    float outdoorTemp;
    float roomSetpoint;

    // Heating
    bool  heatingOn;

    // Fan: 0=off  1=eco  2=high  3–12=level 1–10
    int   fanMode;

    // Boiler: 0=off  1=eco(40°C)  2=high(60°C)  3=boost
    int   boilerMode;

    // Energy: 0=Gas  1=Gas+Elec850  2=Gas+Elec1700  3=Elec850  4=Elec1700
    int   energyIdx;

    // Connectivity
    bool        wifiOk;
    bool        linOk;
    const char* ssid;
    const char* ip;

    // Peripherals
    P4SolarData solar;
    P4BattData  batt;
};

// ── Public API ────────────────────────────────────────────────────────────

// Call once from app_main (before any task uses LVGL).
void p4DisplayInit();

// Call whenever any value changes. Thread-safe: acquires LVGL lock internally.
void p4DisplayUpdate(const P4DisplayData& d);

// Update the status bar message (e.g. "Conectando…", "LIN OK").
void p4DisplaySetStatus(const char* msg);

// LVGL mutex — thin wrappers around bsp_display_lock/unlock.
// timeout_ms = portMAX_DELAY (0xFFFFFFFF) to block forever.
bool lvglLock(uint32_t timeout_ms = portMAX_DELAY);
void lvglUnlock();
