#pragma once
#include <stdint.h>
#include <string>

struct VictronData {
    float    battV;      // battery voltage [V]   (0 = invalid)
    float    battA;      // battery current [A]   (positive = charging)
    float    pvW;        // PV power [W]
    float    kWhToday;   // yield today [kWh]
    uint8_t  state;      // device state (0=off, 3=bulk, 4=absorption, 5=float …)
    uint8_t  errCode;    // Victron error code
    bool     valid;      // true after at least one successful decryption
    uint32_t lastMs;     // esp_timer_get_time()/1000 at last valid reception
};

// Discovered BLE device (used by one-shot settings scan).
struct BleDevice {
    char    name[64];
    char    mac[13];     // "AABBCCDDEEFF\0"
    bool     is_victron; // manufacturer ID matches Victron (0x02E1)
    int8_t   rssi;       // strongest advert RSSI [dBm] this scan (0 = unknown)
    uint16_t seen;       // adverts heard this scan (reception-quality metric)
};

typedef void (*BleDiscoveryCb)(const BleDevice* devs, int count, void* user);

// Call once from app_main. Loads NVS config only — does NOT bring NimBLE up.
void victronBleInit();

// Bring NimBLE up and start the supervisor task. Call AFTER victronBleInit()
// and ultimatronBleInit() have loaded their configs.
void bleSupervisorStart();
// Tear NimBLE down to reclaim internal DRAM before a self-OTA.  Blocks until the
// supervisor exits; false on timeout.  RAM returns via the post-OTA reboot.
bool bleSupervisorStop();

// One-shot discovery scan for settings UI. Runs 8 s scan in a background
// task; cb is invoked from that task (NOT LVGL thread) with results.
// Use lv_async_call inside cb to update LVGL safely.
// victron_only=true → only devices with manufacturer ID 0x02E1.
// victron_only=false → all devices NOT matching Victron (i.e. battery candidates).
void bleDiscoveryScan(bool victron_only, BleDiscoveryCb cb, void* user, uint32_t duration_ms = 8000);

// Reload NVS config and apply immediately (after settings save).
void victronBleReloadConfig();

VictronData victronGetData();
bool        victronIsConfigured();
void        victronBleSuspend();
void        victronBleResume();
bool        victronBleSuspended();
// True while the supervisor is inside a scan window (so a freshly-set suspend
// hasn't taken the radio off-air yet).  Let callers wait for it to clear.
bool        victronBleScanActive();
void        victronBleSetAggressive(bool aggressive);
void        victronBleStop();
void        victronBleStart();

bool victronLoadConfig(std::string& addr, std::string& key);
void victronSaveConfig(const std::string& addr, const std::string& key);
