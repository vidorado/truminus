#pragma once
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include <stdint.h>
#include "control_state.hpp"

// Shared font set — available after p4DisplayInit() returns.
struct P4Fonts {
    const lv_font_t* f18;
    const lv_font_t* f20;
    const lv_font_t* f22;
    const lv_font_t* f24;
    const lv_font_t* f28;
    const lv_font_t* title;    // bold 26
    const lv_font_t* icons22;
    const lv_font_t* icons24;
    const lv_font_t* icons36;  // larger icons for menu buttons
};
const P4Fonts* p4GetFonts();

// ── Data structs fed to p4DisplayUpdate() ────────────────────────────────

struct P4SolarData {
    bool        valid;    // false → display all fields as "--"
    const char* status;   // "Bulk" / "Absorción" / "Float" / "Apagado" / ...
    float       voltageV;
    float       currentA;
    int         powerW;
    float       kWhToday;  // yield today [kWh]
};

struct P4BattData {
    bool  valid;      // false → display SOC and bar as "--" / empty
    bool  flowValid;  // false → SOC still shown, but charge/discharge power as "--"
    int   soc;        // 0–100 %
    float voltageV;
    float currentA;   // pack current [A]; V*A > 0 = charging (web convention)
};

struct P4TankData {
    bool    valid;    // false → display "-- %" and empty fill
    uint8_t pct;      // 0–100 % — fresh-water tank fill level (BTHome)
};

struct P4MultiData {
    bool        valid;        // false → render everything as "--"
    uint8_t     deviceState;  // VE.Bus OperationMode (0xFF = no data)
    const char* stateName;    // localised label for `deviceState` (or nullptr)
    int32_t     acInW;        // shore power [W]
    int32_t     acOutW;       // load power [W]
    float       battV;        // battery voltage [V] (NaN = no data)
    float       battA;        // battery current [A] (positive = charging)
    uint8_t     soc;          // 0..100 %, 0xFF = no data
    uint8_t     alarm;        // 0..2; 3 = no data
    uint8_t     acInState;    // 0=AC1, 1=AC2, 2=disconnected, 3=no data
};

struct P4DisplayData {
    // Temperatures — value < -100 means "invalid / no data"
    float roomTemp;
    float waterTemp;
    float outdoorTemp;
    float roomSetpoint;

    // Heating
    bool  heatingOn;

    // Fan: 0=off  1=eco  2=high  3–12=level 1–10
    int   fanMode;

    // Boiler: 0=off  1=eco(40°C)  2=high(60°C)  3=boost
    int   boilerMode;

    // Burner running for hot water, from LIN frame 0x22. Makes the topbar drop
    // blink, the water analogue of acCompressorOn/the flame — without it the
    // LCD can only show requested-vs-not while the web shows real demand.
    bool  waterHeating;

    // Energy: 0=Gas  1=Gas+Elec850  2=Gas+Elec1700  3=Elec850  4=Elec1700
    int   energyIdx;

    // Connectivity
    bool        wifiOk;
    bool        linOk;
    const char* ssid;
    const char* ip;

    // "Attempt started" flags, set as each subsystem comes up in bootTask's
    // sequence.  A status icon stays dim (DISABLED) until its flag is set, then
    // blinks (CONNECTING) until it connects or the grace window expires (FAILED).
    bool        wifiAttempting;
    bool        linAttempting;
    bool        bleAttempting;

    // BLE status: 0=not configured, 1=configured but no data, 2=has data
    int bleState;

    // OpenAir PLUS A/C configured (NVS "openair"/"addr" non-empty).  When
    // true the CALEFACCIÓN panel becomes CLIMATIZACIÓN with cool/eco/heat/off
    // mode buttons and the FAN panel gains the A/C Auto/Man control.
    bool openairConfigured;

    // OpenAir A/C confirmed by BLE (at least one valid telemetry poll).  Gates
    // the topbar snowflake brightness the same way linOk gates the flame: the
    // glyph follows the selected mode, but stays dim until the unit responds.
    bool acConnected;

    // OpenAir A/C compressor running (telemetry CompressorSpeedRPM > 0).  Makes
    // the topbar snowflake blink, analogous to the flame while the burner fires.
    bool acCompressorOn;

    // Peripherals
    P4SolarData solar;
    P4BattData  batt;
    P4TankData  tank;
    P4MultiData multi;
};

// ── Control state — reflects user button presses ─────────────────────────
// P4ControlState lives in control_state.hpp (IDF-free) so pure logic can be
// host-tested; it is included at the top of this header.

// Returns the current interactive state (modified by on-screen button presses).
// Call before building the next P4DisplayData so the demo loop reflects user input.
void p4GetControlState(P4ControlState& out);

// Setters used by the WebSocket dispatcher to mirror remote button presses
// back onto the LCD.  All setters acquire the LVGL lock internally and call
// refresh_controls() so the on-screen state matches.  Fan and boiler use the
// same integer encoding as p4GetControlState() (see comments in P4DisplayData).
void p4SetHeating(bool on);
void p4SetFanMode(int mode);
void p4SetBoilerMode(int mode);
void p4SetEnergyIdx(int idx);
void p4SetRoomSetpoint(float celsius);

// OpenAir PLUS A/C remote setters (mirror web button presses onto the LCD).
// acMode: 0=off, 1=cool, 2=eco.  Selecting cool/eco clears Truma heating;
// "heat" is driven through p4SetHeating(true) instead.
void p4SetAcMode(int mode);
// A/C fan in cool mode: auto=true → Mode AUTO; auto=false → Mode MAN with
// blower speed 1..6.  Ignored unless the A/C is in cool mode.
void p4SetAcFan(bool autoMode, int speed);
// Louver modes (raw wire value 0/1 each; see OA_FLAP_* in openairble.hpp).
void p4SetAcFlaps(int flap1, int flap2);

// Topbar cloud icon driver.  Pass a TunnelUiState (cast to uint8_t).  On
// CONNECTING the colour toggles each call, so the caller's update cadence
// (≈1 s) becomes the blink rate.
//   0 DISABLED   → dark grey
//   1 CONNECTING → blinking blue/grey
//   2 CONNECTED  → solid blue rgb(70,131,210)
//   3 FAILED     → solid red
void p4SetTunnelState(uint8_t state);

// Show/hide the topbar firmware-update reminder icon (cloud-arrow-down).
// Thread-safe.  Driven from app_main with p4OtaNotify().
void p4SetUpdateAvailable(bool available);

// Show a modal "update now?" prompt over the main screen with Update-now /
// Later buttons.  Returns false (and shows nothing) when the main screen is
// not the active screen or a prompt is already up — the caller should retry
// later in that case.  Thread-safe.
bool p4DisplayShowUpdatePrompt(const char* from_ver, const char* to_ver);

// Show / hide the error-warning modal over the main screen (mirrors the web
// error modal).  title/sub/desc are pre-localised; color is the 0xRRGGBB accent
// for the icon, title and border (red = error, amber = warning).  Show returns
// false (and does nothing) when the main screen isn't active — retry later.
// Both are thread-safe (acquire the LVGL lock internally).
bool p4DisplayShowErrorModal(const char* title, const char* sub,
                             const char* desc, uint32_t color);
void p4DisplayHideErrorModal();

// Close every transient dialog (update prompt + error modal). Called when a
// firmware install starts so no stale dialog floats over the progress UI.
// Thread-safe (acquires the LVGL lock internally).
void p4DisplayDismissDialogs();

// Returns true exactly once after the user taps the modal's "Aceptar" button
// (consumed on read), so the caller can mark that fault acknowledged.
bool p4DisplayErrorModalDismissed();

// ── Public API ────────────────────────────────────────────────────────────

// Call once from app_main (before any task uses LVGL).
void p4DisplayInit();

// Set normal (awake) backlight brightness: 10–100.
// Persists to NVS; recalculates the warning-dim level.
void p4SetNormalBrightness(int pct);

// Apply the screen-timeout selection live (and persist to NVS).
// idx: 0 = 30 s, 1 = 1 min, 2 = 3 min, 3 = never.
// Without this, changes made from the settings screen only take effect on
// reboot because s_timeout_ms is sampled once in p4DisplayInit().
void p4SetScreenTimeoutIdx(uint8_t idx);

// Tear down the current main screen and rebuild it on the next p4DisplayUpdate
// call (picks up a new language, etc.). Must be called with LVGL lock held.
void p4DisplayRebuild();

// Call whenever any value changes. Thread-safe: acquires LVGL lock internally.
void p4DisplayUpdate(const P4DisplayData& d);

// Update the status-bar top line. Pass isError=true to paint it red
// (e.g. when reporting an error code or a hard failure).
void p4DisplaySetStatus(const char* msg, bool isError = false);

// Show a dedicated full-screen OTA progress display.
// from_ver / to_ver: version strings shown as "vX.Y.Z -> vX.Y.Z" (prefix_v=false
// drops the "v", e.g. for web-asset content hashes). title defaults to the
// firmware-update title. Call once before starting the OTA transfer.
void p4DisplayShowOtaScreen(const char* from_ver, const char* to_ver,
                            const char* title = nullptr, bool prefix_v = true);

// Update the progress bar on the OTA screen (0–100).
// Thread-safe; can be called from the main task while LVGL runs on its own task.
void p4DisplaySetOtaProgress(int percent);

// Restore the main screen after a failed self-OTA (a successful one reboots).
// Thread-safe.
void p4DisplayHideOtaScreen();

// LVGL mutex — thin wrappers around bsp_display_lock/unlock.
// timeout_ms = portMAX_DELAY (0xFFFFFFFF) to block forever.
bool lvglLock(uint32_t timeout_ms = portMAX_DELAY);
void lvglUnlock();
