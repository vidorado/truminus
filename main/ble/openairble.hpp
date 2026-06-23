#pragma once
#include <stdint.h>
#include <string>

class NimBLEAdvertisedDevice;

// Telemetry received via GATT notifications from the OpenAir PLUS unit.
// Fields 13-27 of the Mensaje frame (read-only half of the wire format).
// Probe temperatures: raw value units unknown — the official app never
// displays Sonda1C/2C (parsed but not shown), so there is no raw→°C conversion
// to copy; the scaling needs a btsnoop capture against the unit.  Surfaced raw.
struct OpenAirData {
    float    probe1C;            // Sonda1C — probe 1 temperature
    float    probe2C;            // Sonda2C — probe 2 temperature
    int      errors;             // Errors bitfield
    int      batteryValue;       // BatteryValue field
    int      blowerSpeedPct;     // BlowerSpeedPer [%]
    int      compressorSpeedRpm; // CompressorSpeedRPM
    bool     valid;
    uint32_t lastMs;
};

// Command to apply on the next GATT connection.
// Set via openairSetCmd(); applied by openairPollOnce().
// Maps from P4ControlState (see main.cpp::buildOpenAirCmd).
struct OpenAirCmd {
    int  powerState;     // 0=OFF, 1=ON
    int  mode;           // 0=AUTO, 1=ECO, 2=MAN  (confirmed from acc_mode_toggle_button)
    int  tempTenths;     // setpoint °C × 10  (e.g. 210 = 21.0 °C; range 160-320)
    int  blowerSpeed;    // fan level 1–6 (used only in MAN mode)
    int  ledBright;      // panel LED brightness 0/1
    int  ledColor;       // panel LED colour index (1/3/10 observed)
    int  scheduledTime;  // timer value (0 = disabled)
    int  flaps1;         // louver 1 mode 0/1
    int  flaps2;         // louver 2 mode 0/1
};

// Call once from bootTask alongside the other BLE *Init() calls.
void openairBleInit();

// Reload NVS config and apply (call after settings save).
void openairBleReloadConfig();

// Fed every BLE advertisement by VictronScanCb so we can tell whether the
// A/C is in range before spending time on a GATT connect attempt.
// Also captures service-data bytes (used in the handshake write).
void     openairBleHandleAd(const NimBLEAdvertisedDevice* dev);
uint32_t openairLastSeenMs();   // milliseconds timestamp; 0 = never seen

bool        openairIsConfigured();
OpenAirData openairGetData();

// Update the pending command (thread-safe).  Called from main.cpp whenever the
// A/C control state changes so the next poll delivers the latest setpoint.
void openairSetCmd(const OpenAirCmd& cmd);

// Called from bleSupervisorTask: connect to the A/C, write the pending
// command, collect one telemetry notification, disconnect.
bool openairPollOnce();

bool openairLoadConfig(std::string& addr);
void openairSaveConfig(const std::string& addr);

// English title for an OpenAir `Errors` value (decoded from the app's
// listaErrores; see the openair-plus skill).  Returns nullptr for 0 (no fault)
// or any unrecognised code, so callers can use it as a "show this?" predicate.
const char* openairErrorTitle(int errors);

// Localised long description for the error modal body (nullptr = no fault).
const char* openairErrorDesc(int errors);
