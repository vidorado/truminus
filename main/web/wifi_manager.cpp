#include "wifi_manager.hpp"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "wifi";

#define MAX_RETRY          5        // fast back-to-back retries on a drop
#define WIFI_SLOW_RETRY_MS 30000    // then keep retrying forever at this cadence
#define MAX_APS            20

static WifiStatus         s_status     = {};
static SemaphoreHandle_t  s_statusMux  = nullptr;
static int                s_retryCount = 0;
static bool               s_hasCreds   = false;
static bool               s_started    = false;
static esp_timer_handle_t s_retryTimer = nullptr;   // slow-retry timer (periodic)
static bool               s_slowRetry  = false;      // slow retry currently armed

// Fires every WIFI_SLOW_RETRY_MS once the fast retries are exhausted, so the
// device recovers from any prolonged outage (AP/router reboot, ISP down) without
// a power cycle. Stopped the moment we get an IP.
static void wifi_retry_timer_cb(void*) {
    if (s_hasCreds) {
        ESP_LOGI(TAG, "slow reconnect attempt");
        esp_wifi_connect();
    }
}

static void stop_slow_retry(void) {
    if (s_slowRetry && s_retryTimer) esp_timer_stop(s_retryTimer);
    s_slowRetry = false;
}

static void status_set(bool connected, const char* ssid, const char* ip) {
    xSemaphoreTake(s_statusMux, portMAX_DELAY);
    s_status.connected = connected;
    if (ssid) { snprintf(s_status.ssid, sizeof(s_status.ssid), "%s", ssid); }
    if (ip)   { snprintf(s_status.ip,   sizeof(s_status.ip),   "%s", ip);   }
    xSemaphoreGive(s_statusMux);
}

static void wifi_event_handler(void* arg, esp_event_base_t base,
                                int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (s_hasCreds) {
                ESP_LOGI(TAG, "STA started, connecting…");
                esp_wifi_connect();
            }
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            auto* ev = (wifi_event_sta_disconnected_t*)data;
            ESP_LOGW(TAG, "disconnected (reason=%d), retry %d/%d",
                     ev->reason, s_retryCount + 1, MAX_RETRY);
            status_set(false, nullptr, nullptr);
            if (s_hasCreds && s_retryCount < MAX_RETRY) {
                s_retryCount++;
                esp_wifi_connect();
            } else if (s_hasCreds && !s_slowRetry && s_retryTimer) {
                // Fast retries exhausted — don't give up; fall back to slow
                // periodic retries forever until the AP/Internet is back.
                ESP_LOGW(TAG, "fast retries exhausted — slow retry every %d s",
                         WIFI_SLOW_RETRY_MS / 1000);
                s_slowRetry = true;
                esp_timer_start_periodic(s_retryTimer,
                                         (uint64_t)WIFI_SLOW_RETRY_MS * 1000);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = (ip_event_got_ip_t*)data;
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "got IP %s", ip_str);

        wifi_config_t cfg = {};
        esp_wifi_get_config(WIFI_IF_STA, &cfg);
        status_set(true, (const char*)cfg.sta.ssid, ip_str);
        s_retryCount = 0;
        stop_slow_retry();   // back online — drop the slow-retry cadence
    }
}

void wifi_manager_init(void) {
    s_statusMux = xSemaphoreCreateMutex();

    esp_timer_create_args_t targs = {};
    targs.callback = wifi_retry_timer_cb;
    targs.name     = "wifi_retry";
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retryTimer));

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
}

void wifi_manager_start(void) {
    // Load saved credentials from NVS.
    nvs_handle_t h;
    char ssid[33] = {}, pass[65] = {};
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(ssid); nvs_get_str(h, "ssid", ssid, &len);
        len = sizeof(pass);        nvs_get_str(h, "pass", pass, &len);
        nvs_close(h);
    }

    if (strlen(ssid) > 0) {
        wifi_config_t wifi_cfg = {};
        snprintf((char*)wifi_cfg.sta.ssid,     sizeof(wifi_cfg.sta.ssid),     "%s", ssid);
        snprintf((char*)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s", pass);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        s_hasCreds = true;
        snprintf(s_status.ssid, sizeof(s_status.ssid), "%s", ssid);
        ESP_LOGI(TAG, "saved creds: ssid='%s'", ssid);
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    // Keep the WiFi radio always awake (no modem sleep).  Two reasons, both
    // measured on this C6/esp_hosted board:
    //   1. Throughput/latency: MIN_MODEM throttled bulk transfers badly
    //      (OTA crawled at ~30 KB/s), web UI janky, tunnel laggy.
    //   2. BLE coexistence: counter-intuitively, MIN_MODEM made BLE RX *worse*
    //      — a phone advert visible at point-blank under PS_NONE vanished
    //      entirely under MIN_MODEM.  On this shared front end, modem sleep
    //      disrupts the BLE scan windows rather than freeing airtime for them.
    // So PS_NONE wins for both WiFi throughput AND BLE.  Don't switch to
    // MIN_MODEM expecting a BLE win — it's the opposite here.
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps != ESP_OK) ESP_LOGW(TAG, "esp_wifi_set_ps(NONE) failed: %s", esp_err_to_name(ps));

    s_started = true;
}

void wifi_manager_connect(const char* ssid, const char* pass) {
    // Save to NVS.
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass ? pass : "");
        nvs_commit(h);
        nvs_close(h);
    }

    wifi_config_t wifi_cfg = {};
    snprintf((char*)wifi_cfg.sta.ssid,     sizeof(wifi_cfg.sta.ssid),     "%s", ssid);
    snprintf((char*)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s", pass ? pass : "");

    s_hasCreds   = true;
    s_retryCount = 0;
    stop_slow_retry();   // new credentials — restart the fast-retry cycle
    snprintf(s_status.ssid, sizeof(s_status.ssid), "%s", ssid);

    if (s_started) {
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        esp_wifi_connect();
    } else {
        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        esp_wifi_start();
        s_started = true;
    }
}

// ── Async scan ────────────────────────────────────────────────────────────

struct ScanTaskCtx {
    WifiScanCb cb;
    void*      user;
};

static void scan_task_v2(void* arg) {
    auto* ctx = (ScanTaskCtx*)arg;
    WifiScanCb cb   = ctx->cb;
    void*      user = ctx->user;
    free(ctx);

    if (!s_started) {
        cb(nullptr, 0, user);
        vTaskDelete(nullptr);
        return;
    }

    esp_err_t err = esp_wifi_scan_start(nullptr, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start failed: %s", esp_err_to_name(err));
        cb(nullptr, 0, user);
        vTaskDelete(nullptr);
        return;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    ESP_LOGI(TAG, "scan found %u APs", count);
    if (count > MAX_APS) count = (uint16_t)MAX_APS;

    WifiAP* aps = nullptr;
    if (count > 0) {
        wifi_ap_record_t records[MAX_APS];
        esp_wifi_scan_get_ap_records(&count, records);
        aps = (WifiAP*)malloc(count * sizeof(WifiAP));
        for (uint16_t i = 0; i < count; i++) {
            snprintf(aps[i].ssid, sizeof(aps[i].ssid), "%s", (const char*)records[i].ssid);
            aps[i].rssi = records[i].rssi;
        }
    }

    cb(aps, (int)count, user);
    free(aps);
    vTaskDelete(nullptr);
}

void wifi_manager_scan_async(WifiScanCb cb, void* user_data) {
    auto* ctx = (ScanTaskCtx*)malloc(sizeof(ScanTaskCtx));
    ctx->cb   = cb;
    ctx->user = user_data;
    xTaskCreate(scan_task_v2, "wifi_scan", 4096, ctx, 2, nullptr);
}

WifiStatus wifi_manager_get_status(void) {
    xSemaphoreTake(s_statusMux, portMAX_DELAY);
    WifiStatus copy = s_status;
    xSemaphoreGive(s_statusMux);
    return copy;
}
