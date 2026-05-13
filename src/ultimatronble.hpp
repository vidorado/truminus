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

// Call once from setup() before WiFi.begin(). No-op if NVS not configured.
void ultimatronBleInit();

UltimatronData ultimatronGetData();
bool          ultimatronIsConfigured();

void ultimatronBleSuspend();
void ultimatronBleResume();

// NVS helpers used by wifisetup.
// `pass`: 6 ASCII digits (e.g. "999000"). Empty string = no authentication step.
bool ultimatronLoadConfig(String& addr, String& pass);
void ultimatronSaveConfig(const String& addr, const String& pass);
