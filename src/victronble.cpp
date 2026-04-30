#ifdef CYD
#include "victronble.hpp"
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <mbedtls/aes.h>

// ── NVS ───────────────────────────────────────────────────────────────────
static const char* NVS_NS   = "solar";
static const char* NVS_ADDR = "addr";   // 12 uppercase hex chars, no separators
static const char* NVS_KEY  = "key";    // 32 uppercase hex chars

bool victronLoadConfig(String& addr, String& key) {
    Preferences p;
    p.begin(NVS_NS, true);
    if (!p.isKey(NVS_ADDR)) { p.end(); return false; }
    addr = p.getString(NVS_ADDR, "");
    key  = p.getString(NVS_KEY,  "");
    p.end();
    return addr.length() == 12 && key.length() == 32;
}

void victronSaveConfig(const String& addr, const String& key) {
    Preferences p;
    p.begin(NVS_NS, false);
    p.putString(NVS_ADDR, addr);
    p.putString(NVS_KEY,  key);
    p.end();
}

// ── Module state ──────────────────────────────────────────────────────────
static bool          s_configured = false;
static uint8_t       s_aesKey[16] = {};   // decoded from 32-char hex
static String        s_targetAddr;        // 12 uppercase hex chars (no colons)

static SemaphoreHandle_t s_dataMux = nullptr;
static VictronData       s_data    = {};

static BLEScan*      s_bleScan      = nullptr;
static TaskHandle_t  s_bleTaskHandle = nullptr;

// ── Hex helpers ───────────────────────────────────────────────────────────
static bool hexToBytes(const String& hex, uint8_t* out, int len) {
    if ((int)hex.length() < len * 2) return false;
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

// Strip colons and uppercase — "aa:bb:cc:dd:ee:ff" → "AABBCCDDEEFF"
static String normaliseAddr(const std::string& raw) {
    String s;
    for (char c : raw) {
        if (c != ':') s += (char)toupper((unsigned char)c);
    }
    return s;
}

// ── AES-128-CTR decrypt via mbedtls ──────────────────────────────────────
static bool aesCtrDecrypt(const uint8_t* cipher, int len,
                          uint8_t iv0, uint8_t iv1, uint8_t* out) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, s_aesKey, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return false;
    }
    uint8_t nonce[16]        = {};
    uint8_t stream_block[16] = {};
    size_t  nc_off           = 0;
    nonce[0] = iv0;
    nonce[1] = iv1;
    int ret = mbedtls_aes_crypt_ctr(&ctx, (size_t)len, &nc_off,
                                    nonce, stream_block, cipher, out);
    mbedtls_aes_free(&ctx);
    return ret == 0;
}

// ── Parse manufacturer-specific advertisement data ────────────────────────
// Victron format (26+ bytes):
//   [0-1]  Company ID  0xE1 0x02
//   [2]    Record type 0x10 = Solar Controller
//   [3-6]  Misc / reserved
//   [7-8]  IV (little-endian 16-bit counter)
//   [9]    Reserved
//   [10-25] 16-byte AES-CTR ciphertext
//
// Decrypted output[0..9]:
//   [0]    Device state
//   [1]    Error code
//   [2-3]  Battery voltage  ×10 mV  (uint16 LE, 0xFFFF = invalid)
//   [4-5]  Battery current  ×100 mA (int16 LE,  0x7FFF = invalid)
//   [6-7]  Yield today      ×10 Wh  (uint16 LE)
//   [8-9]  PV power         W       (uint16 LE)
static void parseMfrData(const uint8_t* mfr, int len) {
    if (len < 26) return;
    if (mfr[0] != 0xE1 || mfr[1] != 0x02) return;
    if (mfr[2] != 0x10) return;   // solar controller record type

    uint8_t out[16] = {};
    if (!aesCtrDecrypt(mfr + 10, 16, mfr[7], mfr[8], out)) return;

    uint16_t rawV  = (uint16_t)out[2] | ((uint16_t)out[3] << 8);
    int16_t  rawA  = (int16_t)((uint16_t)out[4] | ((uint16_t)out[5] << 8));
    uint16_t rawKwh = (uint16_t)out[6] | ((uint16_t)out[7] << 8);
    uint16_t rawPv  = (uint16_t)out[8] | ((uint16_t)out[9] << 8);

    if (rawV == 0xFFFF) return;   // invalid voltage = decryption probably failed

    VictronData d;
    d.state    = out[0];
    d.errCode  = out[1];
    d.battV    = rawV  * 0.01f;
    d.battA    = (rawA == 0x7FFF) ? 0.0f : rawA * 0.1f;
    d.kWhToday = rawKwh * 0.01f;
    d.pvW      = (float)rawPv;
    d.valid    = true;
    d.lastMs   = millis();

    if (s_dataMux) xSemaphoreTake(s_dataMux, portMAX_DELAY);
    s_data = d;
    if (s_dataMux) xSemaphoreGive(s_dataMux);
}

// ── BLE callback ──────────────────────────────────────────────────────────
class VictronScanCb : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice dev) override {
        if (!dev.haveManufacturerData()) return;

        // Filter by MAC if configured
        if (s_targetAddr.length() > 0) {
            String devAddr = normaliseAddr(dev.getAddress().toString());
            if (devAddr != s_targetAddr) return;
        }

        std::string raw = dev.getManufacturerData();
        parseMfrData((const uint8_t*)raw.data(), (int)raw.size());
    }
};

// ── Periodic scan task (Core 1) ───────────────────────────────────────────
static void bleTask(void* /*arg*/) {
    vTaskDelay(pdMS_TO_TICKS(8000));   // wait for WiFi/MQTT to finish init
    for (;;) {
        if (s_bleScan) {
            s_bleScan->clearResults();
            s_bleScan->start(3, false);   // 3-second scan, non-blocking
            // Sleep long enough for the scan to finish before we clear results again
            vTaskDelay(pdMS_TO_TICKS(4000));
        }
        vTaskDelay(pdMS_TO_TICKS(26000));   // 26+4 = 30 s total period
    }
}

// ── Public API ────────────────────────────────────────────────────────────
bool victronIsConfigured() { return s_configured; }

VictronData victronGetData() {
    VictronData copy = {};
    if (s_dataMux) {
        xSemaphoreTake(s_dataMux, portMAX_DELAY);
        copy = s_data;
        xSemaphoreGive(s_dataMux);
    }
    return copy;
}

void victronBleInit() {
    String addr, keyHex;
    if (!victronLoadConfig(addr, keyHex)) return;
    if (!hexToBytes(keyHex, s_aesKey, 16)) {
        Serial.println("[ble] bad encryption key — not 32 valid hex chars");
        return;
    }

    s_targetAddr  = addr;   // already uppercase from NVS
    s_configured  = true;
    s_dataMux     = xSemaphoreCreateMutex();

    BLEDevice::init("");
    s_bleScan = BLEDevice::getScan();
    s_bleScan->setAdvertisedDeviceCallbacks(new VictronScanCb(), /*wantDuplicates=*/true);
    s_bleScan->setActiveScan(false);   // passive — no connection needed
    s_bleScan->setInterval(100);
    s_bleScan->setWindow(99);

    xTaskCreate(bleTask, "ble_vic", 4096, nullptr, 1, &s_bleTaskHandle);
    Serial.printf("[ble] Victron BLE init ok, target=%s\n", s_targetAddr.c_str());
}

void victronBleSuspend() {
    if (s_bleTaskHandle) {
        vTaskSuspend(s_bleTaskHandle);
        if (s_bleScan) s_bleScan->stop();
    }
}

#endif // CYD
