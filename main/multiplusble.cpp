// Victron VE.Bus (Multiplus) Instant Readout receiver.
//
// Same outer envelope as Victron Solar — see victronble.cpp::parseMfrData
// — but the encrypted payload is a packed bit-stream described by the
// VE.Bus record type (0x0C) in the Victron BLE Instant Readout spec.
//
// Byte map of the manufacturer-data field (validated against the
// Fabian-Schmidt/esphome-victron_ble C struct):
//   [0..1]  company id           0x02E1 LE
//   [2]     mfr_record_type      0x10 = PRODUCT_ADVERTISEMENT
//   [3]     mfr_record_length
//   [4..5]  product_id           uint16 LE
//   [6]     readout_type         0x01=solar, 0x0C=VE.Bus, …
//   [7..8]  data counter / IV    uint16 LE (AES-CTR nonce)
//   [9]     key check byte       must equal s_aesKey[0]
//   [10..]  AES-128-CTR cipher   16 bytes (we read all 16)
//
// VE.Bus plaintext bit-layout (LSB-first stream):
//    8b  device_state           sentinel 0xFF
//    8b  error                  sentinel 0xFF
//   16b  battery_current (s)    0.1 A,  sentinel 0x7FFF
//   14b  battery_voltage (u)    0.01 V, sentinel 0x3FFF
//    2b  ac_in_state            sentinel 3
//   19b  ac_in_power (s)        1 W
//   19b  ac_out_power (s)       1 W
//    2b  alarm                  sentinel 3
//    7b  battery_temperature    1 °C with -40 offset, sentinel 0x7F
//    7b  state_of_charge        1 %, sentinel 0x7F
//   = 102 bits used out of 128 in the AES block.

#include "multiplusble.hpp"
#include "multiplus_codec.hpp"
#include "logs.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "aes/esp_aes.h"

#include "NimBLEDevice.h"
#include "NimBLEAdvertisedDevice.h"

#include <cmath>
#include <cstring>
#include <cctype>

static const char* TAG = "multiplus";

namespace {

// ── State ────────────────────────────────────────────────────────────────
SemaphoreHandle_t s_dataMux  = nullptr;
MultiplusData     s_data     = {};
bool              s_configured = false;
std::string       s_targetAddr;       // uppercase, no separators
uint8_t           s_aesKey[16] = {};
uint16_t          s_lastIv     = 0xFFFF;
bool              s_haveLastIv = false;

// ── Helpers (mirror victronble.cpp) ─────────────────────────────────────
bool hexToBytes(const std::string& hex, uint8_t* out, int len) {
    if ((int)hex.size() < len * 2) return false;
    for (int i = 0; i < len; i++) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nibble(hex[i * 2]);
        int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

std::string normaliseAddr(const std::string& raw) {
    std::string s;
    for (char c : raw) if (c != ':') s += (char)toupper((unsigned char)c);
    return s;
}

uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

bool aesCtrDecrypt(const uint8_t* cipher, int len,
                   uint8_t iv0, uint8_t iv1, uint8_t* out) {
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    if (esp_aes_setkey(&ctx, s_aesKey, 128) != 0) {
        esp_aes_free(&ctx);
        return false;
    }
    uint8_t nonce[16]        = {};
    uint8_t stream_block[16] = {};
    size_t  nc_off           = 0;
    nonce[0] = iv0; nonce[1] = iv1;
    int rc = esp_aes_crypt_ctr(&ctx, (size_t)len, &nc_off,
                               nonce, stream_block, cipher, out);
    esp_aes_free(&ctx);
    return rc == 0;
}

// The bitstream reader (BitReader) and the plaintext unpacker
// (multiplusParseRecord) live in the IDF-free multiplus_codec.{hpp,cpp} module
// so they can be host-tested against the real code.

// ── NVS load ────────────────────────────────────────────────────────────
void load_config_internal() {
    nvs_handle_t h;
    s_configured = false;
    s_targetAddr.clear();
    memset(s_aesKey, 0, sizeof(s_aesKey));
    if (nvs_open("multiplus", NVS_READONLY, &h) != ESP_OK) return;

    char addr[32] = {};
    size_t alen = sizeof(addr);
    char key[64] = {};
    size_t klen = sizeof(key);
    bool haveAddr = (nvs_get_str(h, "addr", addr, &alen) == ESP_OK);
    bool haveKey  = (nvs_get_str(h, "key",  key,  &klen) == ESP_OK);
    nvs_close(h);

    if (!haveAddr || !haveKey) return;
    if (!hexToBytes(std::string(key), s_aesKey, 16)) return;
    s_targetAddr = normaliseAddr(std::string(addr));
    s_configured = true;
}

}  // namespace

// ── Public API ──────────────────────────────────────────────────────────

void multiplusBleInit() {
    if (!s_dataMux) s_dataMux = xSemaphoreCreateMutex();
    load_config_internal();
    if (s_configured) {
        LOG_BLE_PF("[multi] cfg loaded, target=%s\n", s_targetAddr.c_str());
    } else {
        LOG_BLE_PL("[multi] not configured");
    }
}

void multiplusBleReloadConfig() {
    load_config_internal();
    s_haveLastIv = false;
    if (s_configured) {
        LOG_BLE_PF("[multi] cfg reloaded, target=%s\n", s_targetAddr.c_str());
    } else {
        LOG_BLE_PL("[multi] config cleared");
    }
}

bool multiplusIsConfigured() { return s_configured; }

MultiplusData multiplusGetData() {
    MultiplusData out = {};
    if (s_dataMux && xSemaphoreTake(s_dataMux, pdMS_TO_TICKS(10)) == pdTRUE) {
        out = s_data;
        xSemaphoreGive(s_dataMux);
    }
    return out;
}

bool multiplusLoadConfig(std::string& addr, std::string& key) {
    nvs_handle_t h;
    if (nvs_open("multiplus", NVS_READONLY, &h) != ESP_OK) return false;
    char a[32] = {}; size_t alen = sizeof(a);
    char k[64] = {}; size_t klen = sizeof(k);
    bool ok = (nvs_get_str(h, "addr", a, &alen) == ESP_OK) &&
              (nvs_get_str(h, "key",  k, &klen) == ESP_OK);
    nvs_close(h);
    if (ok) { addr = a; key = k; }
    return ok;
}

void multiplusSaveConfig(const std::string& addr, const std::string& key) {
    nvs_handle_t h;
    if (nvs_open("multiplus", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open RW failed");
        return;
    }
    if (addr.empty() && key.empty()) {
        nvs_erase_key(h, "addr");
        nvs_erase_key(h, "key");
    } else {
        std::string norm = normaliseAddr(addr);
        nvs_set_str(h, "addr", norm.c_str());
        nvs_set_str(h, "key",  key.c_str());
    }
    nvs_commit(h);
    nvs_close(h);
}

void multiplusBleHandleAd(const NimBLEAdvertisedDevice* dev) {
    if (!s_configured || !dev) return;
    if (!dev->haveManufacturerData()) return;

    // Cheap MAC filter first.
    std::string devAddr = normaliseAddr(dev->getAddress().toString());
    if (devAddr != s_targetAddr) return;

    std::string mfrStr = dev->getManufacturerData();
    // VE.Bus Instant Readout packs only 102 plaintext bits, so Victron sends
    // just 13 cipher bytes → a 23-byte manufacturer field (10 header + 13
    // cipher), NOT the 16-byte/26-byte cipher the Solar record uses.  The old
    // `< 26` check silently dropped every real Multiplus advert.
    if (mfrStr.size() < 23) return;       // 10 header + 13 cipher (102 bits)
    const uint8_t* mfr = (const uint8_t*)mfrStr.data();

    if (mfr[0] != 0xE1 || mfr[1] != 0x02) return;   // Victron company id
    if (mfr[2] != 0x10) return;                      // PRODUCT_ADVERTISEMENT
    if (mfr[6] != 0x0C) return;                      // VE.Bus record type
    if (mfr[9] != s_aesKey[0]) return;               // key check byte

    // IV dedup: every VE.Bus frame within a few seconds repeats with the
    // same counter, so processing the first one and skipping the rest
    // keeps the WS broadcast diff filter calm.
    uint16_t iv = (uint16_t)mfr[7] | ((uint16_t)mfr[8] << 8);
    if (s_haveLastIv && iv == s_lastIv) return;

    // Decrypt only the cipher bytes actually present (13 for VE.Bus, up to 16).
    // AES-CTR is a stream cipher so a partial block is fine; the unused tail of
    // pt[] stays zero and the BitReader only consumes the 102 bits it needs.
    int cipherLen = (int)mfrStr.size() - 10;
    if (cipherLen > 16) cipherLen = 16;
    uint8_t pt[16] = {};
    if (!aesCtrDecrypt(mfr + 10, cipherLen, mfr[7], mfr[8], pt)) return;

    MultiplusData d = {};
    multiplusParseRecord(pt, d);
    d.lastMs = now_ms();

    if (s_dataMux && xSemaphoreTake(s_dataMux, portMAX_DELAY) == pdTRUE) {
        s_data = d;
        xSemaphoreGive(s_dataMux);
    }
    s_lastIv     = iv;
    s_haveLastIv = true;

    LOG_BLE_PF("[multi] st=%u err=%u acIn=%dW acOut=%dW V=%.2f A=%.1f soc=%u\n",
               (unsigned)d.deviceState, (unsigned)d.error,
               (int)d.acInW, (int)d.acOutW,
               isnan(d.battV) ? 0.0 : d.battV, d.battA,
               (unsigned)d.soc);
}

const char* multiplusStateName(uint8_t state) {
    // Subset of VE.Bus OperationMode that's interesting to surface in the
    // UI.  See victron-ble/src/devices/vebus.py for the full list — values
    // not enumerated here just render as "—" upstream.
    switch (state) {
        case 0:   return "Off";
        case 1:   return "Low Power";
        case 2:   return "Fault";
        case 3:   return "Bulk";
        case 4:   return "Absorption";
        case 5:   return "Float";
        case 6:   return "Storage";
        case 7:   return "Equalize";
        case 8:   return "Passthru";
        case 9:   return "Inverting";
        case 10:  return "Power Assist";
        case 11:  return "Power Supply";
        case 252: return "External Ctrl";
        case 0xFF:return nullptr;
        default:  return nullptr;
    }
}
