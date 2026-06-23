#include "c6_ota.hpp"
#include "esp_log.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_hosted_host_fw_ver.h"
#include "esp_hosted_api_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "c6_ota";

#ifdef C6_FW_AVAILABLE
// IDF EMBED_FILES uses the basename only for the symbol name.
extern const uint8_t c6_fw_start[] asm("_binary_network_adapter_bin_start");
extern const uint8_t c6_fw_end[]   asm("_binary_network_adapter_bin_end");
#endif

bool c6OtaNeeded(char* slave_ver, size_t slave_ver_len)
{
    esp_hosted_coprocessor_fwver_t sv = {};
    if (esp_hosted_get_coprocessor_fwversion(&sv) != ESP_OK) {
        ESP_LOGW(TAG, "cannot read C6 version — assuming OTA needed");
        if (slave_ver && slave_ver_len) snprintf(slave_ver, slave_ver_len, "?");
        return true;
    }

    if (slave_ver && slave_ver_len)
        snprintf(slave_ver, slave_ver_len, "%u.%u.%u",
                 (unsigned)sv.major1, (unsigned)sv.minor1, (unsigned)sv.patch1);

    uint32_t host  = ESP_HOSTED_VERSION_VAL(ESP_HOSTED_VERSION_MAJOR_1,
                                             ESP_HOSTED_VERSION_MINOR_1,
                                             ESP_HOSTED_VERSION_PATCH_1);
    uint32_t slave = ESP_HOSTED_VERSION_VAL(sv.major1, sv.minor1, sv.patch1);

    // Compare major.minor only; patch differences are compatible.
    // FORCE_C6_OTA: always return true regardless of version (for UI testing only).
#ifdef FORCE_C6_OTA
    bool mismatch = true;
    (void)host; (void)slave;
#else
    bool mismatch = (host & 0xFFFF00u) != (slave & 0xFFFF00u);
#endif
    if (mismatch) {
        ESP_LOGW(TAG, "C6 version mismatch: host=%u.%u slave=%u.%u — OTA needed",
                 ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1,
                 (unsigned)sv.major1, (unsigned)sv.minor1);
    } else {
        ESP_LOGI(TAG, "C6 version OK: %u.%u.%u",
                 (unsigned)sv.major1, (unsigned)sv.minor1, (unsigned)sv.patch1);
    }
    return mismatch;
}

bool c6OtaPerform(void (*progress_cb)(int))
{
#ifndef C6_FW_AVAILABLE
    ESP_LOGW(TAG, "C6 firmware not embedded — run 'make build-c6' then rebuild TruMinus");
    return false;
#else
    size_t fw_size = (size_t)(c6_fw_end - c6_fw_start);
    if (fw_size < 256) {
        ESP_LOGE(TAG, "embedded C6 binary too small (%u bytes) — was it built?", (unsigned)fw_size);
        return false;
    }

    ESP_LOGI(TAG, "starting C6 OTA — %u bytes", (unsigned)fw_size);

    if (esp_hosted_slave_ota_begin() != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed");
        return false;
    }

    const size_t CHUNK = 1024;
    const uint8_t* ptr = c6_fw_start;
    size_t remaining = fw_size;
    int pct_prev = -1;

    while (remaining > 0) {
        size_t n = (remaining > CHUNK) ? CHUNK : remaining;
        // esp_hosted_slave_ota_write takes non-const; data is read-only in practice.
        if (esp_hosted_slave_ota_write(const_cast<uint8_t*>(ptr), (uint32_t)n) != ESP_OK) {
            ESP_LOGE(TAG, "ota_write failed at offset %u", (unsigned)(fw_size - remaining));
            esp_hosted_slave_ota_end();
            return false;
        }
        ptr       += n;
        remaining -= n;

        int pct = (int)((fw_size - remaining) * 100u / fw_size);
        if (pct / 5 != pct_prev / 5) {
            ESP_LOGI(TAG, "C6 OTA: %d%%", pct);
            if (progress_cb) progress_cb(pct);
            pct_prev = pct;
        }
    }

    if (esp_hosted_slave_ota_end() != ESP_OK) {
        ESP_LOGE(TAG, "ota_end failed");
        return false;
    }

    // activate() reboots the slave; supported on slave FW >= 2.6.0.
    esp_hosted_coprocessor_fwver_t sv = {};
    if (esp_hosted_get_coprocessor_fwversion(&sv) == ESP_OK) {
        if (sv.major1 > 2 || (sv.major1 == 2 && sv.minor1 > 5)) {
            ESP_LOGI(TAG, "sending activate to C6");
            esp_hosted_slave_ota_activate();
        }
    }

    ESP_LOGI(TAG, "C6 OTA complete — host must restart to resync");
    return true;
#endif
}
