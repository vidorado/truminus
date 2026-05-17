#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "globals.hpp"
#include "logs.hpp"
#include "p4display.hpp"

static const char* TAG = "main";

// ── Global definitions (declared extern in globals.hpp) ───────────────────
httpd_handle_t  httpServer   = nullptr;
std::atomic<int> wsClientCount{0};
QueueHandle_t   wsQueue      = nullptr;

// ── Prototypes (uncomment as modules are ported) ──────────────────────────
// void linBusTask(void* arg);
// void startWifi(void);
// void startWebServer(void);
// void bleStart(void);

extern "C" void app_main(void)
{
    // NVS must be first — WiFi/BLE/settings all need it.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Default event loop — required by WiFi, HTTP.
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_LOGI(TAG, "TruMinus P4 — starting (heap=%lu)",
             (unsigned long)esp_get_free_heap_size());

    // Display first so the user sees progress on screen immediately.
    p4DisplayInit();

    // TODO: bleStart();
    // TODO: startWifi();
    // TODO: startWebServer();
    // TODO: xTaskCreatePinnedToCore(linBusTask, "lin", 4096, nullptr, 5, nullptr, 0);

    // Demo loop: show placeholder data while other modules are not yet ported.
    P4DisplayData demo = {};
    demo.roomTemp      = -999.0f;   // invalid → "--"
    demo.waterTemp     = -999.0f;
    demo.outdoorTemp   = -999.0f;
    demo.roomSetpoint  = 20.0f;
    demo.heatingOn     = false;
    demo.fanMode       = 0;
    demo.boilerMode    = 0;
    demo.energyIdx     = 0;
    demo.wifiOk        = false;
    demo.linOk         = false;
    demo.ssid          = nullptr;
    demo.ip            = nullptr;
    demo.solar.status  = "Apagado";
    demo.solar.voltageV = 0.0f;
    demo.solar.currentA = 0.0f;
    demo.solar.powerW   = 0;
    demo.batt.soc       = 0;
    demo.batt.voltageV  = 0.0f;

    // Trigger the main screen build (replaces splash).
    p4DisplayUpdate(demo);
    p4DisplaySetStatus("Esperando módulos...");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
