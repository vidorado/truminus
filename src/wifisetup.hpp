#pragma once
#ifdef CYD
#include <Arduino.h>
#include <esp32_smartdisplay.h>

// -----------------------------------------------------------------------
// Touch calibration (NVS)
// -----------------------------------------------------------------------
bool loadTouchCalibration(touch_calibration_data_t& cal);
void saveTouchCalibration(const touch_calibration_data_t& cal);

// Show 3-point calibration screen; sets global touch_calibration_data and saves to NVS.
void runTouchCalibration();

// -----------------------------------------------------------------------
// WiFi credentials (NVS)
// -----------------------------------------------------------------------
bool loadWifiCredentials(String& ssid, String& pass);
void saveWifiCredentials(const String& ssid, const String& pass);

// Show WiFi setup screen. Blocks until connected or cancelled.
// Returns true if connected+saved, false if cancelled/skipped.
bool runWifiSetup(String& ssid, String& pass);

// -----------------------------------------------------------------------
// MQTT config (NVS)
// -----------------------------------------------------------------------
bool loadMqttConfig(String& host, String& port, String& user, String& pass);
void saveMqttConfig(const String& host, const String& port,
                    const String& user, const String& pass);

// Show MQTT setup screen. Blocks until saved or cancelled.
// Returns true if saved (new config), false if cancelled/skipped.
// Fills uri/user/pass only when returning true.
bool runMqttSetup(String& uri, String& user, String& pass);

// -----------------------------------------------------------------------
// Solar (Victron BLE) config (NVS)
// -----------------------------------------------------------------------
// addr: 12 uppercase hex chars (no separators), e.g. "AABBCCDDEEFF"
// key:  32 uppercase hex chars (128-bit AES key from VictronConnect)
bool loadSolarConfig(String& addr, String& key);
void saveSolarConfig(const String& addr, const String& key);

// Show solar config screen. Blocks until saved or cancelled.
// Returns true if saved, false if cancelled/skipped.
bool runSolarSetup(String& addr, String& key);

// -----------------------------------------------------------------------
// Battery (Ultimatron BLE) config screen
// -----------------------------------------------------------------------
// addr: 12 uppercase hex chars (no separators), e.g. "AABBCCDDEEFF"
// pass: 6 ASCII digits (e.g. "999000"), or empty if BMS has no auth set.
bool loadBattConfig(String& addr, String& pass);
void saveBattConfig(const String& addr, const String& pass);

#endif // CYD
