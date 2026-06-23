#pragma once
#include <stdint.h>
#include <stdbool.h>

struct WifiStatus {
    bool connected;
    char ssid[33];
    char ip[16];
};

struct WifiAP {
    char   ssid[33];
    int8_t rssi;
};

typedef void (*WifiScanCb)(const WifiAP* aps, int count, void* user_data);

// Call once from app_main after nvs_flash_init + esp_netif_init.
void wifi_manager_init(void);

// Load saved NVS credentials and start STA mode. Call after wifi_manager_init.
void wifi_manager_start(void);

// Save credentials to NVS and (re)connect. Safe to call from any task.
void wifi_manager_connect(const char* ssid, const char* pass);

// Start a non-blocking WiFi scan. cb is called from a FreeRTOS task
// (NOT the LVGL thread) when results are ready. Use lv_async_call to
// update the UI from within cb.
void wifi_manager_scan_async(WifiScanCb cb, void* user_data);

// Thread-safe status snapshot.
WifiStatus wifi_manager_get_status(void);
