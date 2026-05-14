#pragma once
#include <Arduino.h>

struct UltimatronData {
    uint8_t  soc;       // state of charge [%] 0-100
    float    battV;     // pack voltage [V]
    float    battA;     // current [A] (negative = charging in BMS convention)
    float    tempC;     // first NTC temperature [°C]
    bool     valid;
    uint32_t lastMs;
};

// Call once from setup(). Loads NVS config only — does NOT initialise NimBLE.
// The BLE supervisor in victronble.cpp owns the controller lifecycle.
void ultimatronBleInit();

UltimatronData ultimatronGetData();
bool          ultimatronIsConfigured();

// Internal API used by the BLE supervisor (victronble.cpp).
// Assumes NimBLEDevice::init() has already been called and the controller
// is up. Connects, queries, parses, and disconnects. Returns true on success.
bool ultimatronPollOnce();

// Legacy API — kept as no-ops so wifisetup.cpp keeps compiling. The lazy
// supervisor handles serialisation internally.
void ultimatronBleSuspend();
void ultimatronBleResume();

// NVS helpers used by wifisetup.
// `pass`: 6 ASCII digits (e.g. "999000"). Empty string = no authentication step.
bool ultimatronLoadConfig(String& addr, String& pass);
void ultimatronSaveConfig(const String& addr, const String& pass);
