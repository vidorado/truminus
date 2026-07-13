#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "logs.hpp"
#include "i18n.hpp"
#include "p4display.hpp"
#include "faultlog.hpp"
#include "crashcatch.hpp"
#include "flags.h"
#include "openair_config.hpp"
#include "boot_sequence.hpp"
#include "display_sync.hpp"
#include "ws_router.hpp"
#include "ws_snapshot.hpp"

extern "C" void app_main(void)
{
    // NVS first — WiFi/BLE/settings all need it.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Capture why we last reset (panic/abort/WDT vs controlled/power) before
    // anything else can reboot the chip.
    faultLogInit();
    crashCatchInit();   // log the RTC-captured backtrace, if any

    // Default event loop and netif — required by WiFi.
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    // Apply per-TAG log levels (silences web/wstunnel/ledc/httpd by default;
    // see main/core/flags.h to re-enable or tune).  Must run before any other
    // subsystem emits its first message.
    flags_apply_log_levels();

    // Pick up persisted UI language before the display builds any labels.
    loadLanguage();

    // Cache the OpenAir A/C configured flag once (re-read only on save).
    openairCfgReload();

    ESP_LOGI("main", "TruMinus P4 — starting (heap=%lu)",
             (unsigned long)esp_get_free_heap_size());

    // Display first so the user sees pixels as soon as the panel is up.
    // Everything below this point runs in the background to keep the
    // splash visible without artificial padding: p4DisplayUpdate() in the
    // main loop enforces a 2 s minimum splash; if the background boot
    // takes longer, the splash naturally stays until it finishes (the
    // main screen still renders, just with empty fields that fill in as
    // each subsystem comes online).
    p4DisplayInit();
    // (No boot status here: the status bar belongs to build_main_screen, which
    // runs after the splash — setting it now would target a not-yet-built label.)

    // Spawn the heavy init in a background task so the splash is on screen
    // immediately and the LVGL refresh task is not starved by app_main.
    bootStart(wsOnCommand, wsOnConnected);

    P4DisplayData d = {};
    displaySyncInit(d);
    p4DisplayUpdate(d);

    uint32_t iter = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        iter++;
        displaySyncTick(d, iter);
    }
}
