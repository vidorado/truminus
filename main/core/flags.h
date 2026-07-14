#pragma once
//
// TruMinus — central build flags & log-level switches.
//
// Auto-included in every TU via `-include` in main/CMakeLists.txt; editing
// this file recompiles only main/ — no full rebuild needed.
//
// Three sections:
//   1. Dummy / development modes (synthetic data when hardware is absent).
//   2. Legacy LOG_<CAT>_ENABLED switches — gate the LOG_BLE_PF / LOG_ULT_PF
//      / … helpers in logs.hpp.  Only relevant for files that still use the
//      old macros (victronble.cpp, ultimatronble.cpp).
//   3. Runtime ESP_LOG levels per TAG.  Most active code uses
//      ESP_LOGI(TAG, …) directly, so this is the section to edit when a
//      subsystem is too noisy in the serial monitor.  Applied at boot by
//      flags_apply_log_levels().
//

// ── 1. Dummy / development modes ─────────────────────────────────────────
//
// #define ENABLE_WIFI_DUMMY
// #define ENABLE_SOLAR_DUMMY     // synthetic solar/battery values
// #define ENABLE_BOILER_DUMMY    // oscillating boiler temperature
// #define ENABLE_TEMP_DUMMY      // oscillating room/outdoor temperature
// #define FORCE_C6_OTA           // force C6 reflash even when versions match
// #define LIN_SNIFF_ONLY         // passive LIN bus listener — no TX at all.
                                  // Use with a real CP-Plus as master via RJ12
                                  // splitter to capture the live conversation.
#define HEAP_DIAG              // internal-DRAM bring-up probes (heapdiag.cpp):
                                  // logs free/min/largest INTERNAL + free PSRAM
                                  // with per-stage deltas at each subsystem
                                  // bring-up boundary. Uncomment to A/B PSRAM
                                  // steering knobs. Off = compiles away to nothing.

// ── 2. Per-category legacy log enables (logs.hpp) ────────────────────────
//
// Gates the LOG_<CAT>_PF / LOG_<CAT>_PL / LOG_<CAT>_P helpers.  Disabled by
// default; the macros expand to ((void)0).  Uncomment to compile in.
//
// #define LOG_BLE_ENABLED         // Victron BLE / Multiplus / tank — chatty per advert
// #define LOG_ULT_ENABLED         // Ultimatron BMS — chatty per poll
// #define LOG_WIFI_ENABLED        // (not used by current wifi_manager — see §3)
// #define LOG_WEB_ENABLED         // (not used by current webserver  — see §3)
// #define LOG_LIN_ENABLED         // dormant legacy LIN frame layer
// #define LOG_SETTINGS_ENABLED    // dormant settings.cpp (not yet ported to IDF)
// #define LOG_MEM_ENABLED
// #define LOG_LVGL_ENABLED
// #define LOG_SNIFF_ENABLED
// #define LOG_AM2301_ENABLED      // external AM2301/DHT22 — per-read temp/RH
// #define LOG_CLI_ENABLED

// ── 3. Runtime ESP_LOG levels per TAG ────────────────────────────────────
//
// Wrapped in __ASSEMBLER__ guard because main/CMakeLists.txt -include's this
// header into every TU including the embedded-font .S blobs, and esp_log.h
// is C-only.  Use the ESP_LOG_* symbolic levels:
//   ESP_LOG_NONE  ESP_LOG_ERROR  ESP_LOG_WARN
//   ESP_LOG_INFO  ESP_LOG_DEBUG  ESP_LOG_VERBOSE
//
// Active TAGs (grep `static const char\* TAG` in main/*.cpp):
//   "main", "wifi", "web", "wstunnel", "truma_lin", "lin",
//   "display", "cfg", "c6_ota"
//
// Built-in IDF subsystems that spam at INFO get clamped too.

#ifndef __ASSEMBLER__
#include "esp_log.h"

// — TruMinus subsystems (our own code) —
#ifndef LOG_LEVEL_MAIN
#define LOG_LEVEL_MAIN       ESP_LOG_INFO
#endif
#ifndef LOG_LEVEL_WIFI
#define LOG_LEVEL_WIFI       ESP_LOG_INFO
#endif
#ifndef LOG_LEVEL_TRUMA_LIN
#define LOG_LEVEL_TRUMA_LIN  ESP_LOG_WARN
#endif
#ifndef LOG_LEVEL_LIN
#define LOG_LEVEL_LIN        ESP_LOG_INFO
#endif
#ifndef LOG_LEVEL_DISPLAY
#define LOG_LEVEL_DISPLAY    ESP_LOG_INFO
#endif
#ifndef LOG_LEVEL_CFG
#define LOG_LEVEL_CFG        ESP_LOG_INFO
#endif
#ifndef LOG_LEVEL_C6_OTA
#define LOG_LEVEL_C6_OTA     ESP_LOG_INFO
#endif
#ifndef LOG_LEVEL_WEB
#define LOG_LEVEL_WEB        ESP_LOG_WARN   /* silence per-request asset chatter */
#endif
#ifndef LOG_LEVEL_WSTUNNEL
#define LOG_LEVEL_WSTUNNEL   ESP_LOG_WARN   /* silence per-stream open/close noise */
#endif

// — IDF infrastructure (clamp INFO chatter that's not actionable) —
#ifndef LOG_LEVEL_HTTPD
/* ERROR, not WARN: empty/probe TCP connections (health checks, port scans)
 * make esp_http_server log "400 Bad Request" / "parse_block: incomplete"
 * at WARN from the httpd_txrx/httpd_parse subtags — benign noise. */
#define LOG_LEVEL_HTTPD      ESP_LOG_ERROR
#endif
#ifndef LOG_LEVEL_LEDC
#define LOG_LEVEL_LEDC       ESP_LOG_ERROR  /* backlight GPIO "conflict" warning */
#endif

// — Display BSP / panel / touch / LVGL boot prints —
#ifndef LOG_LEVEL_BSP
#define LOG_LEVEL_BSP        ESP_LOG_WARN
#endif

// — ESP-Hosted (C6 coprocessor) SDIO + RPC chatter —
#ifndef LOG_LEVEL_HOSTED
#define LOG_LEVEL_HOSTED     ESP_LOG_WARN
#endif

// — Network stack: netif/IP, mbedTLS bundle, WS client, NimBLE init —
#ifndef LOG_LEVEL_NET
#define LOG_LEVEL_NET        ESP_LOG_WARN
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Call once, very early in app_main, before any subsystem logs.
void flags_apply_log_levels(void);

#ifdef __cplusplus
}
#endif
#endif  // __ASSEMBLER__
