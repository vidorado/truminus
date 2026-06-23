#pragma once
// Category-based logging via ESP_LOG macros.
// Enable a category with -DLOG_<CAT>_ENABLED in build_flags.
// When disabled the macros expand to ((void)0) and the compiler elides them.
//
// _PF(fmt, ...) — printf-style with format string
// _PL(msg)      — print a const char* or std::string::c_str() with newline
// _P(msg)       — same as _PL (ESP_LOG always appends newline)

#include "esp_log.h"

#define LOG_NOP(...) ((void)0)

// ── BLE — Victron BLE listener ──────────────────────────────────────────
#ifdef LOG_BLE_ENABLED
  #define LOG_BLE_PF(fmt, ...)  ESP_LOGI("ble",  fmt, ##__VA_ARGS__)
  #define LOG_BLE_PL(msg)       ESP_LOGI("ble",  "%s", msg)
  #define LOG_BLE_P(msg)        ESP_LOGI("ble",  "%s", msg)
#else
  #define LOG_BLE_PF(...)  LOG_NOP()
  #define LOG_BLE_PL(...)  LOG_NOP()
  #define LOG_BLE_P(...)   LOG_NOP()
#endif

// ── ULT — Ultimatron BMS ────────────────────────────────────────────────
#ifdef LOG_ULT_ENABLED
  #define LOG_ULT_PF(fmt, ...)  ESP_LOGI("ult",  fmt, ##__VA_ARGS__)
  #define LOG_ULT_PL(msg)       ESP_LOGI("ult",  "%s", msg)
  #define LOG_ULT_P(msg)        ESP_LOGI("ult",  "%s", msg)
#else
  #define LOG_ULT_PF(...)  LOG_NOP()
  #define LOG_ULT_PL(...)  LOG_NOP()
  #define LOG_ULT_P(...)   LOG_NOP()
#endif

// ── WIFI — WiFi events, IP, MAC ─────────────────────────────────────────
#ifdef LOG_WIFI_ENABLED
  #define LOG_WIFI_PF(fmt, ...) ESP_LOGI("wifi", fmt, ##__VA_ARGS__)
  #define LOG_WIFI_PL(msg)      ESP_LOGI("wifi", "%s", msg)
  #define LOG_WIFI_P(msg)       ESP_LOGI("wifi", "%s", msg)
#else
  #define LOG_WIFI_PF(...)  LOG_NOP()
  #define LOG_WIFI_PL(...)  LOG_NOP()
  #define LOG_WIFI_P(...)   LOG_NOP()
#endif

// ── WEB — HTTP / WebSocket ──────────────────────────────────────────────
#ifdef LOG_WEB_ENABLED
  #define LOG_WEB_PF(fmt, ...)  ESP_LOGI("web",  fmt, ##__VA_ARGS__)
  #define LOG_WEB_PL(msg)       ESP_LOGI("web",  "%s", msg)
  #define LOG_WEB_P(msg)        ESP_LOGI("web",  "%s", msg)
#else
  #define LOG_WEB_PF(...)  LOG_NOP()
  #define LOG_WEB_PL(...)  LOG_NOP()
  #define LOG_WEB_P(...)   LOG_NOP()
#endif

// ── LIN — LIN bus / Truma frames / WaterBoost ───────────────────────────
#ifdef LOG_LIN_ENABLED
  #define LOG_LIN_PF(fmt, ...)  ESP_LOGI("lin",  fmt, ##__VA_ARGS__)
  #define LOG_LIN_PL(msg)       ESP_LOGI("lin",  "%s", msg)
  #define LOG_LIN_P(msg)        ESP_LOGI("lin",  "%s", msg)
#else
  #define LOG_LIN_PF(...)  LOG_NOP()
  #define LOG_LIN_PL(...)  LOG_NOP()
  #define LOG_LIN_P(...)   LOG_NOP()
#endif

// ── SETTINGS — settings validation ─────────────────────────────────────
#ifdef LOG_SETTINGS_ENABLED
  #define LOG_SETTINGS_PF(fmt, ...) ESP_LOGI("set", fmt, ##__VA_ARGS__)
  #define LOG_SETTINGS_PL(msg)      ESP_LOGI("set", "%s", msg)
  #define LOG_SETTINGS_P(msg)       ESP_LOGI("set", "%s", msg)
#else
  #define LOG_SETTINGS_PF(...)  LOG_NOP()
  #define LOG_SETTINGS_PL(...)  LOG_NOP()
  #define LOG_SETTINGS_P(...)   LOG_NOP()
#endif

// ── MEM — heap / PSRAM / stack watermarks ───────────────────────────────
#ifdef LOG_MEM_ENABLED
  #define LOG_MEM_PF(fmt, ...)  ESP_LOGI("mem",  fmt, ##__VA_ARGS__)
  #define LOG_MEM_PL(msg)       ESP_LOGI("mem",  "%s", msg)
  #define LOG_MEM_P(msg)        ESP_LOGI("mem",  "%s", msg)
#else
  #define LOG_MEM_PF(...)  LOG_NOP()
  #define LOG_MEM_PL(...)  LOG_NOP()
  #define LOG_MEM_P(...)   LOG_NOP()
#endif

// ── LVGL — display / LVGL internals ─────────────────────────────────────
#ifdef LOG_LVGL_ENABLED
  #define LOG_LVGL_PF(fmt, ...) ESP_LOGI("lvgl", fmt, ##__VA_ARGS__)
  #define LOG_LVGL_PL(msg)      ESP_LOGI("lvgl", "%s", msg)
  #define LOG_LVGL_P(msg)       ESP_LOGI("lvgl", "%s", msg)
#else
  #define LOG_LVGL_PF(...)  LOG_NOP()
  #define LOG_LVGL_PL(...)  LOG_NOP()
  #define LOG_LVGL_P(...)   LOG_NOP()
#endif

// ── SNIFF — raw LIN sniffer ─────────────────────────────────────────────
#ifdef LOG_SNIFF_ENABLED
  #define LOG_SNIFF_PF(fmt, ...) ESP_LOGI("sniff", fmt, ##__VA_ARGS__)
  #define LOG_SNIFF_PL(msg)      ESP_LOGI("sniff", "%s", msg)
  #define LOG_SNIFF_P(msg)       ESP_LOGI("sniff", "%s", msg)
#else
  #define LOG_SNIFF_PF(...)  LOG_NOP()
  #define LOG_SNIFF_PL(...)  LOG_NOP()
  #define LOG_SNIFF_P(...)   LOG_NOP()
#endif

// ── AM2301 — external DHT sensor ────────────────────────────────────────
#ifdef LOG_AM2301_ENABLED
  #define LOG_AM2301_PF(fmt, ...) ESP_LOGI("am2301", fmt, ##__VA_ARGS__)
  #define LOG_AM2301_PL(msg)      ESP_LOGI("am2301", "%s", msg)
  #define LOG_AM2301_P(msg)       ESP_LOGI("am2301", "%s", msg)
#else
  #define LOG_AM2301_PF(...)  LOG_NOP()
  #define LOG_AM2301_PL(...)  LOG_NOP()
  #define LOG_AM2301_P(...)   LOG_NOP()
#endif

// ── CLI — serial CLI echo / help ────────────────────────────────────────
#ifdef LOG_CLI_ENABLED
  #define LOG_CLI_PF(fmt, ...)  ESP_LOGI("cli",  fmt, ##__VA_ARGS__)
  #define LOG_CLI_PL(msg)       ESP_LOGI("cli",  "%s", msg)
  #define LOG_CLI_P(msg)        ESP_LOGI("cli",  "%s", msg)
#else
  #define LOG_CLI_PF(...)  LOG_NOP()
  #define LOG_CLI_PL(...)  LOG_NOP()
  #define LOG_CLI_P(...)   LOG_NOP()
#endif
