#pragma once
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

// Call once from setup(). Loads NVS config only — does NOT bring NimBLE up.
// The lazy supervisor handles controller lifecycle.
void victronBleInit();

// Start the lazy BLE supervisor task. Call AFTER both victronBleInit() and
// ultimatronBleInit() have loaded their respective NVS configs. The task
// cycles: NimBLE init → scan Victron → poll Ultimatron → NimBLE deinit →
// sleep ~20 s. During the sleep window, internal SRAM is fully available
// so AsyncTCP/LWIP can service web traffic.
void bleSupervisorStart();

VictronData        victronGetData();   // returns a mutex-protected copy
bool               victronIsConfigured();
void               victronBleSuspend(); // pause scan task (e.g. before GATT connection)
void               victronBleResume();  // resume scan task after suspension
// true  = scan every 3 s  (normal, default)
// false = scan every 20 s (power-save when screen is off and no web clients)
void               victronBleSetAggressive(bool aggressive);
// Stop scan completely (release RF for WiFi).  Safe to call multiple times.
void               victronBleStop();
// Resume after victronBleStop().  Safe to call multiple times.
void               victronBleStart();

// NVS helpers (used by wifisetup / solar config screen).
bool victronLoadConfig(String& addr, String& key);
void victronSaveConfig(const String& addr, const String& key);
