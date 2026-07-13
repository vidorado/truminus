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
    int      batteryType;        // BatteryType (bytes 4-5) — chemistry as reported (1=lithium)
    int      power;              // Power (bytes 6-7) — max power 0=2.0 kW, 1=1.2 kW (inverted)
    // Unit-reported control half (write-region fields the unit currently runs).
    // Used to seed/reconcile P4ControlState so the UI mirrors the real unit
    // (on boot, and when the unit is changed by its own remote or the app).
    int      uPowerState;        // PowerState (bytes 10-11) 0=OFF 1=ON
    int      uMode;              // Mode (bytes 12-13) 0=AUTO 1=ECO 2=MAN
    int      uTempTenths;        // Temp setpoint (bytes 14-15) tenths °C
    int      uBlower;            // BlowerSpeed (bytes 16-17) 1-6
    bool     valid;
    uint32_t lastMs;
};

// Dirty-bit flags for OpenAirCmd::configDirty. The config fields (BatteryType,
// Power) are echoed back from the unit's own telemetry on every command UNLESS
// their bit is set here — the app forbids changing them while the unit runs, so
// the Peripherals screen sets these only when the unit is OFF.
enum { OA_CFG_BATTERY = 1 << 0, OA_CFG_POWER = 1 << 1 };

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

    // Config fields — written only when their OA_CFG_* bit is set in configDirty
    // (change only while the unit is OFF); otherwise echoed from telemetry.
    int  batteryType;    // battery chemistry 0=Pb (lead-acid), 1=lithium
    int  power;          // max power 0=2.0 kW, 1=1.2 kW (inverted mapping)
    int  configDirty;    // OA_CFG_BATTERY | OA_CFG_POWER bitmask
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
// True once telemetry has been read for the current target (handshake accepted).
// Reset when the config reloads. The pairing assistant polls this to auto-close.
bool        openairPaired();

// True when live telemetry has arrived recently (within AC_STALE_MS) — i.e. the
// A/C is actually reachable and talking to us right now. Distinct from the
// data's `valid` flag, which stays true holding the last (possibly stale) frame
// after the unit drops off. Drives the "A/C disconnected" UI (struck snowflake +
// bottombar alert).
bool        openairConnected();

// True when the unit is present but has repeatedly refused our handshake — it
// dropped our pairing and is waiting for a long-press on its ON/OFF button (it
// shows "Bt"). Drives the "re-pair" alert on the web + LCD. Cleared by the next
// telemetry read.
bool        openairNeedsPair();
OpenAirData openairGetData();

// Update the pending command (thread-safe).  Called from main.cpp whenever the
// A/C control state changes so the next poll delivers the latest setpoint.
void openairSetCmd(const OpenAirCmd& cmd);

// True while a user command is queued/in-flight. Callers that reconcile the UI
// from telemetry use this to avoid fighting a change the user just made.
bool openairCmdPending();

// Called every bleSupervisorTask cycle. Holds ONE persistent GATT connection
// open (the unit powers off when the link drops), reconnecting only after a
// drop; telemetry arrives via notifications, commands go out on change.
bool openairPollOnce();

bool openairLoadConfig(std::string& addr);
void openairSaveConfig(const std::string& addr);

// English title for an OpenAir `Errors` value (decoded from the app's
// listaErrores; see the openair-plus skill).  Returns nullptr for 0 (no fault)
// or any unrecognised code, so callers can use it as a "show this?" predicate.
const char* openairErrorTitle(int errors);

// Localised long description for the error modal body (nullptr = no fault).
const char* openairErrorDesc(int errors);
