#include "faultlog.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "nvs.h"
#include <cstring>

static const char* TAG = "fault";

// Reset causes that mean the code actually crashed — as opposed to a
// controlled restart (ESP_RST_SW) or a power event (power-on / brownout).
// Only these update the stored record, so real bugs aren't masked.
static bool is_fault(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC   || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_INT_WDT || r == ESP_RST_WDT;
}

const char* faultReasonName(int r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "sw-restart";
        case ESP_RST_PANIC:     return "panic/abort";
        case ESP_RST_INT_WDT:   return "int-wdt";
        case ESP_RST_TASK_WDT:  return "task-wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deep-sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        case ESP_RST_USB:       return "usb";
        case ESP_RST_JTAG:      return "jtag";
        default:                return "unknown";
    }
}

void faultLogInit() {
    esp_reset_reason_t boot = esp_reset_reason();
    ESP_LOGI(TAG, "boot: reset reason = %s (%d)", faultReasonName(boot), (int)boot);

    nvs_handle_t h;
    if (nvs_open("diag", NVS_READWRITE, &h) != ESP_OK) return;

    if (is_fault(boot)) {
        uint32_t cnt = 0;
        nvs_get_u32(h, "f_cnt", &cnt);
        cnt++;
        const esp_app_desc_t* d = esp_app_get_description();
        const char* ver = (d && d->version[0]) ? d->version : "?";
        nvs_set_u8 (h, "f_rsn", (uint8_t)boot);
        nvs_set_u32(h, "f_cnt", cnt);
        nvs_set_str(h, "f_ver", ver);
        nvs_commit(h);
        ESP_LOGE(TAG, "UNCONTROLLED RESET: %s — recorded (count=%lu, fw=%s)",
                 faultReasonName(boot), (unsigned long)cnt, ver);
    }

    // Report whatever fault is on record (it outlives controlled reboots and
    // power cuts), so the cause is visible even without a serial monitor.
    uint8_t rsn = 0;
    if (nvs_get_u8(h, "f_rsn", &rsn) == ESP_OK) {
        uint32_t cnt = 0; nvs_get_u32(h, "f_cnt", &cnt);
        char ver[32] = ""; size_t vl = sizeof(ver); nvs_get_str(h, "f_ver", ver, &vl);
        ESP_LOGW(TAG, "last uncontrolled fault: %s (count=%lu, fw=%s)",
                 faultReasonName((int)rsn), (unsigned long)cnt, ver);
    } else {
        ESP_LOGI(TAG, "no uncontrolled fault on record");
    }
    nvs_close(h);
}

bool faultLogGet(FaultInfo& out) {
    nvs_handle_t h;
    if (nvs_open("diag", NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t rsn = 0;
    bool ok = (nvs_get_u8(h, "f_rsn", &rsn) == ESP_OK);
    if (ok) {
        out.reason  = (esp_reset_reason_t)rsn;
        out.count   = 0;  nvs_get_u32(h, "f_cnt", &out.count);
        out.version[0] = '\0';
        size_t vl = sizeof(out.version);
        nvs_get_str(h, "f_ver", out.version, &vl);
    }
    nvs_close(h);
    return ok;
}
