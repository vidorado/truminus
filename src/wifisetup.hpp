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

// Show WiFi setup screen. Blocks until connected. Fills ssid and pass.
void runWifiSetup(String& ssid, String& pass);

// -----------------------------------------------------------------------
// MQTT config (NVS)
// -----------------------------------------------------------------------
bool loadMqttConfig(String& host, String& port, String& user, String& pass);
void saveMqttConfig(const String& host, const String& port,
                    const String& user, const String& pass);

// Show MQTT setup screen. Blocks until saved.
// Returns assembled uri ("mqtt://host:port"), user and pass.
void runMqttSetup(String& uri, String& user, String& pass);

#endif // CYD
