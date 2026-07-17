#pragma once
#include <stdint.h>
#include <string>

class NimBLEAdvertisedDevice;

struct UltimatronData {
    uint8_t  soc;       // state of charge [%] 0-100
    float    battV;     // pack voltage [V]
    float    battA;     // current [A] (negative = charging in BMS convention)
    float    tempC;     // first NTC temperature [°C]
    bool     valid;
    uint32_t lastMs;    // esp_timer_get_time()/1000 at last valid reception
};

// Call once from app_main. Loads NVS config only.
void ultimatronBleInit();

// Reload NVS config and apply immediately (after settings save).
void ultimatronBleReloadConfig();

UltimatronData ultimatronGetData();
bool           ultimatronIsConfigured();

// Internal: called by bleSupervisorTask in victronble.cpp. With blind=true it
// uses a short connect budget (one 2 s attempt instead of 3×5 s) for the
// recovery poll of a BMS that has stopped advertising, so a failed attempt only
// briefly displaces the passive scans.
bool ultimatronPollOnce(bool blind = false);

// DIAGNOSTIC: fed every advertisement by VictronScanCb so we can tell whether
// the BMS is actually advertising (and thus whether a connect attempt is even
// worth making).  Records the last-seen timestamp when the MAC matches.
void     ultimatronBleHandleAd(const NimBLEAdvertisedDevice* dev);
uint32_t ultimatronLastSeenMs();   // 0 = never seen this boot

void ultimatronBleSuspend();
void ultimatronBleResume();
bool ultimatronBleSuspended();

bool ultimatronLoadConfig(std::string& addr, std::string& pass);
void ultimatronSaveConfig(const std::string& addr, const std::string& pass);
