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

// Internal: called by bleSupervisorTask in victronble.cpp.
bool ultimatronPollOnce();

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
