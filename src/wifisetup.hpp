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

#endif // CYD
