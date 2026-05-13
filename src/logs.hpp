#pragma once
// Logging por categorías.
//
// Cada categoría se activa con su propio flag de compilación
// (`-DLOG_<CAT>_ENABLED`). En `platformio.ini` están todos listados como
// comentarios dentro de `[env]`; descomenta los que necesites en el momento.
//
// Las macros son pass-through a `Serial.printf/println/print` cuando la
// categoría está activa, y `((void)0)` cuando no, así que el compilador
// elide las llamadas sin tocar el binario.

#include <Arduino.h>

#define LOG_NOP(...) ((void)0)

// ────────────────────────────────────────────────────────────
// LOG_BLE  — Victron BLE listener (`[ble]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_BLE_ENABLED
  #define LOG_BLE_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_BLE_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_BLE_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_BLE_PF(...) LOG_NOP()
  #define LOG_BLE_PL(...) LOG_NOP()
  #define LOG_BLE_P(...)  LOG_NOP()
#endif

// ────────────────────────────────────────────────────────────
// LOG_ULT  — Ultimatron BMS (`[ult]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_ULT_ENABLED
  #define LOG_ULT_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_ULT_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_ULT_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_ULT_PF(...) LOG_NOP()
  #define LOG_ULT_PL(...) LOG_NOP()
  #define LOG_ULT_P(...)  LOG_NOP()
#endif

// ────────────────────────────────────────────────────────────
// LOG_BLE_SCAN  — escaneo BLE durante setup (`[scan]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_BLE_SCAN_ENABLED
  #define LOG_BLE_SCAN_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_BLE_SCAN_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_BLE_SCAN_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_BLE_SCAN_PF(...) LOG_NOP()
  #define LOG_BLE_SCAN_PL(...) LOG_NOP()
  #define LOG_BLE_SCAN_P(...)  LOG_NOP()
#endif

// ────────────────────────────────────────────────────────────
// LOG_WIFI  — eventos WiFi, MAC, IP (`[wifi]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_WIFI_ENABLED
  #define LOG_WIFI_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_WIFI_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_WIFI_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_WIFI_PF(...) LOG_NOP()
  #define LOG_WIFI_PL(...) LOG_NOP()
  #define LOG_WIFI_P(...)  LOG_NOP()
#endif

// ────────────────────────────────────────────────────────────
// LOG_WIFI_SETUP  — pantalla LVGL de setup WiFi/MQTT (`[wifisetup]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_WIFI_SETUP_ENABLED
  #define LOG_WIFI_SETUP_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_WIFI_SETUP_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_WIFI_SETUP_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_WIFI_SETUP_PF(...) LOG_NOP()
  #define LOG_WIFI_SETUP_PL(...) LOG_NOP()
  #define LOG_WIFI_SETUP_P(...)  LOG_NOP()
#endif

// ────────────────────────────────────────────────────────────
// LOG_TOUCH  — calibración táctil (`[cal]`, `[touchcal]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_TOUCH_ENABLED
  #define LOG_TOUCH_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_TOUCH_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_TOUCH_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_TOUCH_PF(...) LOG_NOP()
  #define LOG_TOUCH_PL(...) LOG_NOP()
  #define LOG_TOUCH_P(...)  LOG_NOP()
#endif

// ────────────────────────────────────────────────────────────
// LOG_OTA  — Arduino OTA (`[ota]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_OTA_ENABLED
  #define LOG_OTA_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_OTA_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_OTA_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_OTA_PF(...) LOG_NOP()
  #define LOG_OTA_PL(...) LOG_NOP()
  #define LOG_OTA_P(...)  LOG_NOP()
#endif

// ────────────────────────────────────────────────────────────
// LOG_WEB  — HTTP/WebSocket (`[web]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_WEB_ENABLED
  #define LOG_WEB_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_WEB_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_WEB_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_WEB_PF(...) LOG_NOP()
  #define LOG_WEB_PL(...) LOG_NOP()
  #define LOG_WEB_P(...)  LOG_NOP()
#endif

// ────────────────────────────────────────────────────────────
// LOG_LIN  — LIN bus / frames Truma / WaterBoost (`[lin]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_LIN_ENABLED
  #define LOG_LIN_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_LIN_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_LIN_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_LIN_PF(...) LOG_NOP()
  #define LOG_LIN_PL(...) LOG_NOP()
  #define LOG_LIN_P(...)  LOG_NOP()
#endif

// ────────────────────────────────────────────────────────────
// LOG_SETTINGS  — validaciones de settings (`[set]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_SETTINGS_ENABLED
  #define LOG_SETTINGS_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_SETTINGS_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_SETTINGS_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_SETTINGS_PF(...) LOG_NOP()
  #define LOG_SETTINGS_PL(...) LOG_NOP()
  #define LOG_SETTINGS_P(...)  LOG_NOP()
#endif

// ────────────────────────────────────────────────────────────
// LOG_MEM  — heap, PSRAM, watermarks (`[mem]`, `[psram]`, `[wm]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_MEM_ENABLED
  #define LOG_MEM_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_MEM_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_MEM_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_MEM_PF(...) LOG_NOP()
  #define LOG_MEM_PL(...) LOG_NOP()
  #define LOG_MEM_P(...)  LOG_NOP()
#endif

// ────────────────────────────────────────────────────────────
// LOG_LVGL  — info LVGL, splash, logo, smartdisplay (`[lvgl]`, `[logo]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_LVGL_ENABLED
  #define LOG_LVGL_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_LVGL_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_LVGL_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_LVGL_PF(...) LOG_NOP()
  #define LOG_LVGL_PL(...) LOG_NOP()
  #define LOG_LVGL_P(...)  LOG_NOP()
#endif

// ────────────────────────────────────────────────────────────
// LOG_FS  — LittleFS (`[LittleFS]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_FS_ENABLED
  #define LOG_FS_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_FS_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_FS_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_FS_PF(...) LOG_NOP()
  #define LOG_FS_PL(...) LOG_NOP()
  #define LOG_FS_P(...)  LOG_NOP()
#endif

// ────────────────────────────────────────────────────────────
// LOG_SNIFF  — sniffer LIN crudo (`[sniff]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_SNIFF_ENABLED
  #define LOG_SNIFF_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_SNIFF_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_SNIFF_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_SNIFF_PF(...) LOG_NOP()
  #define LOG_SNIFF_PL(...) LOG_NOP()
  #define LOG_SNIFF_P(...)  LOG_NOP()
#endif

// ────────────────────────────────────────────────────────────
// LOG_AM2301  — sensor exterior DHT (`[am2301]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_AM2301_ENABLED
  #define LOG_AM2301_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_AM2301_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_AM2301_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_AM2301_PF(...) LOG_NOP()
  #define LOG_AM2301_PL(...) LOG_NOP()
  #define LOG_AM2301_P(...)  LOG_NOP()
#endif

// ────────────────────────────────────────────────────────────
// LOG_CLI  — eco/help del CLI serie (`[cli]`)
// ────────────────────────────────────────────────────────────
#ifdef LOG_CLI_ENABLED
  #define LOG_CLI_PF(...) Serial.printf(__VA_ARGS__)
  #define LOG_CLI_PL(...) Serial.println(__VA_ARGS__)
  #define LOG_CLI_P(...)  Serial.print(__VA_ARGS__)
#else
  #define LOG_CLI_PF(...) LOG_NOP()
  #define LOG_CLI_PL(...) LOG_NOP()
  #define LOG_CLI_P(...)  LOG_NOP()
#endif
