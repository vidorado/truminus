#include "faultlog.hpp"
#include "faultlog_codec.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "nvs.h"
#include <cstring>

static const char* TAG = "fault";

// faultIsCrash() and faultReasonName() live in the IDF-free faultlog_codec
// module so the classification is host-tested against the real code.

void faultLogInit() {
    esp_reset_reason_t boot = esp_reset_reason();
    ESP_LOGI(TAG, "boot: reset reason = %s (%d)", faultReasonName(boot), (int)boot);

    nvs_handle_t h;
    if (nvs_open("diag", NVS_READWRITE, &h) != ESP_OK) return;

    if (faultIsCrash((int)boot)) {
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
