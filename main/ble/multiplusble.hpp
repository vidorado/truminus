#pragma once
#include <stdint.h>
#include <string>

class NimBLEAdvertisedDevice;

// "Not available" marker for the AC power fields below.  The VE.Bus record
// stores ac_in_power / ac_out_power as 19-bit signed values whose unknown
// sentinel is the maximum positive code (0x3FFFF).  The parser maps that to
// this value so consumers can tell "no reading" (inverter off / not
// reporting) apart from a genuine 0 W.
static constexpr int32_t MULTI_POWER_NA = INT32_MIN;

// Victron Multiplus / Multiplus-II / Phoenix Inverter etc. paired with a
// VE.Bus Smart dongle.  The dongle advertises an Instant Readout record
// with readout_type = 0x0C (VE.Bus).  Same AES-CTR encryption envelope
// as the Victron Solar charger record but a different (per-device) key
// and a packed-bitstream plaintext layout — see parser in .cpp.
struct MultiplusData {
    uint8_t  deviceState;   // OperationMode enum (0xFF = no data)
    uint8_t  error;         // VE.Bus error code (0xFF = none)
    uint8_t  acInState;     // 0..2 enum, 3 = no data
    uint8_t  alarm;         // 0..2 enum, 3 = no data
    int32_t  acInW;         // shore-side AC power [W]    (positive = consumed, MULTI_POWER_NA = no data)
    int32_t  acOutW;        // load-side AC power [W]     (positive = supplied, MULTI_POWER_NA = no data)
    float    battV;         // battery voltage [V]        (NaN if no data)
    float    battA;         // battery current [A]        (+ charging / − inverting)
    int8_t   battTempC;     // battery temperature [°C]  (-128 = no data)
    uint8_t  soc;           // state of charge [%]        (0xFF = no data)
    bool     valid;         // true after at least one successful decryption
    uint32_t lastMs;
};

void multiplusBleInit();
void multiplusBleReloadConfig();

// Called from VictronScanCb::onResult — single shared NimBLE scan callback.
// Validates record type, target MAC, key check, AES-decrypts and unpacks.
void multiplusBleHandleAd(const NimBLEAdvertisedDevice* dev);

MultiplusData multiplusGetData();
bool          multiplusIsConfigured();

bool multiplusLoadConfig(std::string& addr, std::string& key);
void multiplusSaveConfig(const std::string& addr, const std::string& key);

// String helper for the UI: "Inverting" / "Charging" / "Off" / etc.
// Returns nullptr when state has no canonical name.
const char* multiplusStateName(uint8_t state);
