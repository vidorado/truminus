#pragma once
#include <stdint.h>
#include <string>

// Forward-declare NimBLE type to avoid pulling NimBLE headers into all
// translation units that just want to read a snapshot.
class NimBLEAdvertisedDevice;

// Tank-level sensor data parsed from a BTHome v2 advertisement
// (Service UUID 0xFCD2, Moisture tag 0x2F).
struct TankData {
    uint8_t  pct;       // 0..100 — last decoded fill level
    bool     valid;     // true after at least one valid reading
    uint32_t lastMs;    // esp_timer_get_time()/1000 at last valid reception
};

// Call once from app_main. Loads NVS allow-list MAC. Does NOT init NimBLE.
void tankBleInit();

// Re-read NVS after a settings save / CLI update.
void tankBleReloadConfig();

// Plumbed in from VictronScanCb::onResult — a single shared scan callback
// in NimBLE means we cannot register our own.  Parses BTHome service-data
// and stores the value if the source MAC matches the allow-list.
void tankBleHandleAd(const NimBLEAdvertisedDevice* dev);

// Thread-safe snapshot accessor (used by main loop + WS pump).
TankData tankGetData();

// True when an allow-list MAC is configured in NVS.
bool tankIsConfigured();

// Get/set the configured sensor MAC.  `addr` is uppercase no-separator hex
// (e.g. "AABBCCDDEEFF"); empty string clears the binding.
bool tankLoadConfig(std::string& addr);
void tankSaveConfig(const std::string& addr);
