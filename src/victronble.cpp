#include "victronble.hpp"
#include <math.h>
#if defined(BLE)
#include <Preferences.h>
#include <NimBLEDevice.h>
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

static NimBLEScan*   s_bleScan      = nullptr;
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
// Victron "Instant Readout" format — only present in SCAN RESPONSE (active scan).
// Bleak strips the company ID, so relative to raw NimBLE getManufacturerData():
//   [0-1]  Company ID  0xE1 0x02  (stripped by bleak, kept by NimBLE)
//   [2]    0x10  — Instant Readout marker  (= data[0] in Python library)
//   [3]    prefix hi byte
//   [4-5]  Model ID (LE)
//   [6]    Readout type (0x01 = solar charger)
//   [7-8]  IV (little-endian 16-bit counter)
//   [9]    Key-check byte — must equal key[0]
//   [10-N] AES-128-CTR ciphertext (first 16 bytes used)
//
// Decrypted output (LSB-first bit packing, same layout as Python solar_charger.py):
//   [0]    Device state
//   [1]    Error code
//   [2-3]  Battery voltage  (int16 LE, ÷100 → V,  0x7FFF = invalid)
//   [4-5]  Battery current  (int16 LE, ÷10 → A,   0x7FFF = invalid)
//   [6-7]  Yield today      (uint16 LE, ×10 → Wh)
//   [8-9]  PV power         (uint16 LE, W)
static void parseMfrData(const uint8_t* mfr, int len) {
    if (len < 18) return;
    if (mfr[0] != 0xE1 || mfr[1] != 0x02) return;
    if (mfr[2] != 0x10) return;   // Instant Readout marker — only in SCAN_RSP

    // Key-check byte: must match key[0] before attempting decryption
    if (mfr[9] != s_aesKey[0]) return;

    uint8_t out[16] = {};
    if (!aesCtrDecrypt(mfr + 10, 16, mfr[7], mfr[8], out)) return;

    int16_t  rawV  = (int16_t)((uint16_t)out[2] | ((uint16_t)out[3] << 8));
    int16_t  rawA  = (int16_t)((uint16_t)out[4] | ((uint16_t)out[5] << 8));
    uint16_t rawKwh = (uint16_t)out[6] | ((uint16_t)out[7] << 8);
    uint16_t rawPv  = (uint16_t)out[8] | ((uint16_t)out[9] << 8);

    if (rawV == 0x7FFF) return;   // sentinel — no data yet

    VictronData d;
    d.state    = out[0];
    d.errCode  = out[1];
    d.battV    = rawV * 0.01f;
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
class VictronScanCb : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!dev->haveManufacturerData()) return;

        std::string raw = dev->getManufacturerData();
        const uint8_t* mfr = (const uint8_t*)raw.data();
        int mlen = (int)raw.size();


        // Filter by MAC if configured
        if (s_targetAddr.length() > 0) {
            String devAddr = normaliseAddr(dev->getAddress().toString());
            if (devAddr != s_targetAddr) return;
        }

        parseMfrData(mfr, mlen);
    }
};

// ── Periodic scan task (Core 1) ───────────────────────────────────────────
static void bleTask(void* /*arg*/) {
    vTaskDelay(pdMS_TO_TICKS(8000));   // wait for WiFi/MQTT to finish init
    Serial.println("[ble] scan task started");
    for (;;) {
        if (s_bleScan) {
            s_bleScan->clearResults();
            s_bleScan->start(3, false);   // 3-second blocking scan
            vTaskDelay(pdMS_TO_TICKS(2000));   // 3+2 = 5 s total period
        }
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

    Serial.printf("[ble] heap before init: free=%u largest=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    NimBLEDevice::init("");
    Serial.printf("[ble] heap after init:  free=%u\n", ESP.getFreeHeap());
    s_bleScan = NimBLEDevice::getScan();
    s_bleScan->setScanCallbacks(new VictronScanCb(), /*wantDuplicates=*/true);
    s_bleScan->setActiveScan(true);    // active scan needed to receive SCAN_RSP (Instant Readout)
    s_bleScan->setInterval(100);
    s_bleScan->setWindow(99);
    s_bleScan->setMaxResults(0);       // no caching — fire onResult for every adv

    xTaskCreate(bleTask, "ble_vic", 3072, nullptr, 1, &s_bleTaskHandle);
    Serial.printf("[ble] Victron BLE init ok, target=%s\n", s_targetAddr.c_str());
}

void victronBleSuspend() {
    if (s_bleTaskHandle) {
        vTaskSuspend(s_bleTaskHandle);
        if (s_bleScan) s_bleScan->stop();
    }
}

void victronBleResume() {
    if (s_bleTaskHandle) vTaskResume(s_bleTaskHandle);
}

#else // !BLE — simulated data for web UI development

static VictronData s_fakeVictron = {
    13.2f,  5.5f, 120.0f, 1.25f,
    3, 0, true, 0
};

void victronBleInit() {}

VictronData victronGetData() {
    float t = millis() / 1000.0f;
    s_fakeVictron.pvW      = 80.0f + 40.0f * sinf(t * 0.8f);
    s_fakeVictron.battA    = 3.0f + 2.5f * sinf(t * 0.5f);
    s_fakeVictron.battV    = 13.0f + 0.3f * sinf(t * 1.2f);
    s_fakeVictron.kWhToday = 1.25f + 0.01f * t;
    s_fakeVictron.lastMs   = millis();
    return s_fakeVictron;
}

bool victronIsConfigured() { return true; }
void victronBleSuspend() {}
void victronBleResume() {}
bool victronLoadConfig(String& addr, String& key) { (void)addr; (void)key; return false; }
void victronSaveConfig(const String& addr, const String& key) { (void)addr; (void)key; }

#endif // BLE
