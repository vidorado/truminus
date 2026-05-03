#pragma once
#ifdef CYD
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
bool ultimatronLoadConfig(String& addr);
void ultimatronSaveConfig(const String& addr);

#endif // CYD
