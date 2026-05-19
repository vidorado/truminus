#pragma once
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include <stdint.h>

// Shared font set — available after p4DisplayInit() returns.
struct P4Fonts {
    const lv_font_t* f18;
    const lv_font_t* f20;
    const lv_font_t* f22;
    const lv_font_t* f24;
    const lv_font_t* f28;
    const lv_font_t* title;    // bold 26
    const lv_font_t* icons22;
    const lv_font_t* icons24;
    const lv_font_t* icons36;  // larger icons for menu buttons
};
const P4Fonts* p4GetFonts();

// ── Data structs fed to p4DisplayUpdate() ────────────────────────────────

struct P4SolarData {
    bool        valid;    // false → display all fields as "--"
    const char* status;   // "Bulk" / "Absorción" / "Float" / "Apagado" / ...
    float       voltageV;
    float       currentA;
    int         powerW;
};

struct P4BattData {
    bool  valid;      // false → display SOC and bar as "--" / empty
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

    // BLE status: 0=not configured, 1=configured but no data, 2=has data
    int bleState;

    // Peripherals
    P4SolarData solar;
    P4BattData  batt;
};

// ── Control state — reflects user button presses ─────────────────────────

struct P4ControlState {
    bool  heatingOn;
    int   fanMode;
    int   boilerMode;
    int   energyIdx;
    float roomSetpoint;
};

// Returns the current interactive state (modified by on-screen button presses).
// Call before building the next P4DisplayData so the demo loop reflects user input.
void p4GetControlState(P4ControlState& out);

// ── Public API ────────────────────────────────────────────────────────────

// Call once from app_main (before any task uses LVGL).
void p4DisplayInit();

// Set normal (awake) backlight brightness: 10–100.
// Persists to NVS; recalculates the warning-dim level.
void p4SetNormalBrightness(int pct);

// Tear down the current main screen and rebuild it on the next p4DisplayUpdate
// call (picks up a new language, etc.). Must be called with LVGL lock held.
void p4DisplayRebuild();

// Call whenever any value changes. Thread-safe: acquires LVGL lock internally.
void p4DisplayUpdate(const P4DisplayData& d);

// Update the status-bar top line. Pass isError=true to paint it red
// (e.g. when reporting an error code or a hard failure).
void p4DisplaySetStatus(const char* msg, bool isError = false);

// Show a dedicated full-screen OTA progress display.
// from_ver / to_ver: version strings shown as "vX.Y.Z -> vX.Y.Z".
// Call once before starting the OTA transfer.
void p4DisplayShowOtaScreen(const char* from_ver, const char* to_ver);

// Update the progress bar on the OTA screen (0–100).
// Thread-safe; can be called from the main task while LVGL runs on its own task.
void p4DisplaySetOtaProgress(int percent);

// LVGL mutex — thin wrappers around bsp_display_lock/unlock.
// timeout_ms = portMAX_DELAY (0xFFFFFFFF) to block forever.
bool lvglLock(uint32_t timeout_ms = portMAX_DELAY);
void lvglUnlock();
