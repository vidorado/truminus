#pragma once
#ifdef CYD
#include <Arduino.h>

struct VictronData {
    float    battV;      // battery voltage [V]  (0xFFFF raw = invalid)
    float    battA;      // battery current [A]  (positive = charging)
    float    pvW;        // PV power [W]
    float    kWhToday;   // yield today [kWh]
    uint8_t  state;      // device state (0=off, 3=bulk, 4=absorption, 5=float …)
    uint8_t  errCode;    // Victron error code
    bool     valid;      // true after at least one successful decryption
    uint32_t lastMs;     // millis() of last valid reception
};

// Call once from setup() — no-op if "solar" NVS namespace is not configured.
void victronBleInit();

VictronData        victronGetData();   // returns a mutex-protected copy
bool               victronIsConfigured();
void               victronBleSuspend(); // stop scan task before setup screen

// NVS helpers (used by wifisetup / solar config screen).
bool victronLoadConfig(String& addr, String& key);
void victronSaveConfig(const String& addr, const String& key);

#endif // CYD
