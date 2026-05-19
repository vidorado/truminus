#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "globals.hpp"
#include "logs.hpp"
#include "i18n.hpp"
#include "p4display.hpp"
#include "wifi_manager.hpp"
#include "victronble.hpp"
#include "ultimatronble.hpp"
#include "c6_ota.hpp"
#include "webserver.hpp"
#include "esp_hosted_host_fw_ver.h"
extern "C" {
#include "esp_hosted_misc.h"
}
#include <cmath>
#include <stdio.h>

static const char* TAG = "main";

// ── WebSocket command dispatcher ─────────────────────────────────────────
//
// The web UI sends JSON {id, value} frames; in the C5 firmware they were
// routed through settings.cpp (`TMqttSetting::Validate(...)` per id).  That
// layer has not been ported to ESP-IDF yet, so for now we simply log the
// commands and the page acts as read-only.  Wire this to the real settings
// dispatcher once trumaframes / settings are ported.
static void onWsCommand(const char* id, const char* value) {
    ESP_LOGI(TAG, "ws cmd: %s = %s", id ? id : "(null)", value ? value : "(null)");
}

// Browser just connected and sent "settings": push a snapshot of every
// cached value.  Same TODO as above — without settings.cpp ported we have
// no cache to snapshot, so emit a minimal heartbeat.
static void onWsConnected() {
    wsQueueSend("{\"command\":\"snapshot\",\"settings\":{},\"status\":{}}");
}

// Background boot: everything that does not need to block the splash.
// Runs in parallel with the splash screen so the user sees pixels as fast
// as bsp_display_start_with_config() returns.  Each step is independent and
// already non-blocking (BLE supervisor self-spawns, wifi_manager_start is
// non-blocking, mountWebFs / startWebServer are fast).
static void bootTask(void* /*arg*/) {
    // WiFi driver init.  ESP-Hosted transport to the C6 co-processor is
    // established here; the C6 OTA check that follows depends on it.
    wifi_manager_init();

    // C6 co-processor OTA: if the embedded slave firmware version differs
    // from the host ESP-Hosted library (major.minor), reflash the C6 via
    // SDIO and restart.  The OTA screen overrides the splash; the device
    // reboots when it completes, so we never return.
    {
        char slave_ver[16] = "?";
        if (c6OtaNeeded(slave_ver, sizeof(slave_ver))) {
            char host_ver[16];
            snprintf(host_ver, sizeof(host_ver), "%d.%d.%d",
                     ESP_HOSTED_VERSION_MAJOR_1,
                     ESP_HOSTED_VERSION_MINOR_1,
                     ESP_HOSTED_VERSION_PATCH_1);
            p4DisplayShowOtaScreen(slave_ver, host_ver);
            if (c6OtaPerform(p4DisplaySetOtaProgress)) {
                p4DisplaySetOtaProgress(100);
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            }
        }
    }

    wifi_manager_start();

    // Initialize C6 BT controller via ESP-Hosted RPC before NimBLE starts.
    ESP_ERROR_CHECK(esp_hosted_bt_controller_init());
    ESP_ERROR_CHECK(esp_hosted_bt_controller_enable());

    victronBleInit();
    ultimatronBleInit();
    xTaskCreate([](void*) {
        bleSupervisorStart();
        vTaskDelete(nullptr);
    }, "ble_start", 6144, nullptr, 1, nullptr);

    // Web assets live on a LittleFS partition flashed from <project>/data/.
    if (mountWebFs() != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS mount failed — run 'idf.py littlefs-flash-littlefs'");
    }
    startWebServer(onWsCommand, onWsConnected);

    // TODO: xTaskCreatePinnedToCore(linBusTask, "lin", 4096, nullptr, 5, nullptr, 0);

    ESP_LOGI("boot", "background boot complete (heap=%lu)",
             (unsigned long)esp_get_free_heap_size());
    vTaskDelete(nullptr);
}

extern "C" void app_main(void)
{
    // NVS first — WiFi/BLE/settings all need it.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Default event loop and netif — required by WiFi.
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_LOGI(TAG, "TruMinus P4 — starting (heap=%lu)",
             (unsigned long)esp_get_free_heap_size());

    // GPIO 23 is wired to the backlight but LEDC flags it as "conflicted" —
    // cosmetic warning, display works fine. Suppress it.
    esp_log_level_set("ledc", ESP_LOG_ERROR);

    // Display first so the user sees pixels as soon as the panel is up.
    // Everything below this point runs in the background to keep the
    // splash visible without artificial padding: p4DisplayUpdate() in the
    // main loop enforces a 2 s minimum splash; if the background boot
    // takes longer, the splash naturally stays until it finishes (the
    // main screen still renders, just with empty fields that fill in as
    // each subsystem comes online).
    p4DisplayInit();
    p4DisplaySetStatus(t(TK::STATUS_INIT));

    // Spawn the heavy init in a background task so the splash is on screen
    // immediately and the LVGL refresh task is not starved by app_main.
    xTaskCreate(bootTask, "boot", 6144, nullptr, 5, nullptr);

    P4DisplayData d = {};
    d.roomTemp     = -999.0f;
    d.waterTemp    = -999.0f;
    d.outdoorTemp  = -999.0f;
    d.roomSetpoint = 20.0f;

    p4DisplayUpdate(d);

    uint32_t iter = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        iter++;

        // Drain queued WebSocket frames produced by other tasks (LIN, BLE).
        wsQueueDrain();

        // WiFi status
        WifiStatus ws = wifi_manager_get_status();
        d.wifiOk = ws.connected;
        d.ssid   = ws.connected ? ws.ssid : nullptr;
        d.ip     = ws.connected ? ws.ip   : nullptr;

        // Connection info goes to lbl_conn (bottom line) via p4DisplayUpdate.
        // Top status line is for actions/errors only; don't overwrite it here.

        // BLE / solar data
        VictronData    vd = victronGetData();
        UltimatronData ud = ultimatronGetData();

        if (vd.valid) {
            d.solar.valid   = true;
            // Map numeric state to English string (translate_solar_status handles i18n)
            static const char* STATE_STR[] = {
                "Off", "", "Fault", "Bulk", "Absorption", "Float"
            };
            uint8_t st = vd.state;
            d.solar.status   = (st < 6) ? STATE_STR[st] : "Off";
            d.solar.voltageV = vd.battV;
            d.solar.currentA = vd.battA;
            d.solar.powerW   = (int)vd.pvW;
        } else {
            d.solar.valid = false;
        }

        if (ud.valid) {
            d.batt.valid    = true;
            d.batt.soc      = (int)ud.soc;
            d.batt.voltageV = ud.battV;
        } else {
            d.batt.valid = false;
        }

        d.bleState = (vd.valid || ud.valid) ? 2
                   : (victronIsConfigured() || ultimatronIsConfigured()) ? 1
                   : 0;

        // Dummy overrides (in dummy_flags.h)
#ifdef ENABLE_BOILER_DUMMY
        d.waterTemp = 52.0f + 5.0f * sinf((float)iter * 0.3f);
#endif
#ifdef ENABLE_TEMP_DUMMY
        d.roomTemp    = 20.5f + 0.5f * sinf((float)iter * 0.2f);
        d.outdoorTemp = 12.0f + 2.0f * sinf((float)iter * 0.15f);
#endif

        // Reflect interactive button state so user presses are not overwritten.
        P4ControlState cs;
        p4GetControlState(cs);
        d.heatingOn    = cs.heatingOn;
        d.fanMode      = cs.fanMode;
        d.boilerMode   = cs.boilerMode;
        d.energyIdx    = cs.energyIdx;
        d.roomSetpoint = cs.roomSetpoint;

        p4DisplayUpdate(d);
    }
}
