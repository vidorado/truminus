#pragma once
#ifdef CYD
#include <Arduino.h>
#include "settings.hpp"
#include "i18n.hpp"

// Call once in setup(), after initialising the display.
void cydDisplayInit(TTempSetting*   roomSetpoint,
                    TBoilerSetting* waterSetpoint,
                    TOnOffSetting*  heatingOn,
                    TFanSetting*    fanMode);

// Call in loop() — updates status indicators and controls.
void cydDisplayUpdate(bool wifiok, bool mqttok, bool trumaok,
                      bool truma_reset, bool inota,
                      bool mqttEnabled,
                      float   roomTemp     = -273.0f,
                      float   waterTemp    = -273.0f,
                      bool    waterHeating = false,
                      float   extTemp      = -273.0f,
                      uint8_t errClass     = 0,
                      uint8_t errCode      = 0);

// Index of the energy mode selected on-screen (0-4).
// Maps to TEnergySelection: 0=EsGasDiesel … 4=EsElectro1800.
int  cydGetEnergyMode();
// Sync the dropdown from an external source (call under LVGL mutex).
void cydSetEnergyIdx(int idx);

// ── Language ───────────────────────────────────────────────────────────────
// Blocking language-selection screen; saves choice to NVS.
// Safe to call before the LVGL task starts (uses lv_task_handler() directly)
// or from loop() while holding the LVGL mutex.
void cydShowLangSelect();

// Rebuild the main UI in-place after a language change (call under LVGL mutex).
void cydRebuildUI();

// ── Settings / navigation ──────────────────────────────────────────────────
// Settings-screen buttons set this flag from LVGL callbacks.
// main.cpp reads it in loop() OUTSIDE the mutex to safely drive the blocking
// runWifiSetup / runMqttSetup / language-select screens.
enum class CydNavRequest { None, WifiSetup, MqttSetup, LangChange, SolarSetup };
CydNavRequest cydGetNavRequest();
void          cydClearNavRequest();
// Reload s_scr (call after returning from a settings screen).
// Also turns the backlight on and clears any power-off overlay.
void          cydReloadScreen();

// ── Solar data display (call under LVGL mutex from loop()) ─────────────────
struct CydSolarData {
    bool    configured;
    bool    valid;
    float   battV;
    float   battA;
    float   pvW;
    float   kWhToday;
    uint8_t state;
    uint32_t ageMs;    // millis() since last valid reception
};
void cydUpdateSolar(const CydSolarData& d);

#endif // CYD
