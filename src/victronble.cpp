#include "victronble.hpp"
#include "ultimatronble.hpp"
#include "logs.hpp"
#include <math.h>
#if defined(ENABLE_BLE)
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <mbedtls/aes.h>
#include <esp_heap_caps.h>

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
static volatile bool s_aggressive   = true;   // default: fast scan
static volatile bool s_bleStopped   = false;  // true when stop() called
static volatile bool s_bleSuspended = false;  // true when another task needs RF

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
    if (len < 18) {
        LOG_BLE_PF("[ble] adv too short len=%d (need >=18)\n", len);
        return;
    }
    if (mfr[0] != 0xE1 || mfr[1] != 0x02) {
        LOG_BLE_PF("[ble] adv bad company id %02X %02X (want E1 02)\n", mfr[0], mfr[1]);
        return;
    }
    if (mfr[2] != 0x10) {
        LOG_BLE_PF("[ble] adv not Instant Readout marker=%02X (want 10)\n", mfr[2]);
        return;   // Instant Readout marker — only in SCAN_RSP
    }

    // Key-check byte: must match key[0] before attempting decryption
    if (mfr[9] != s_aesKey[0]) {
        LOG_BLE_PF("[ble] key mismatch adv=%02X cfg=%02X\n", mfr[9], s_aesKey[0]);
        return;
    }

    uint8_t out[16] = {};
    if (!aesCtrDecrypt(mfr + 10, 16, mfr[7], mfr[8], out)) {
        LOG_BLE_PL("[ble] AES decrypt failed");
        return;
    }

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

        // Filter by MAC if configured
        if (s_targetAddr.length() > 0) {
            String devAddr = normaliseAddr(dev->getAddress().toString());
            if (devAddr != s_targetAddr) return;
        }

        std::string raw = dev->getManufacturerData();
        const uint8_t* mfr = (const uint8_t*)raw.data();
        int mlen = (int)raw.size();

        LOG_BLE_PF("[ble] adv from %s len=%d head=%02X%02X%02X\n",
                      dev->getAddress().toString().c_str(), mlen,
                      mlen > 0 ? mfr[0] : 0,
                      mlen > 1 ? mfr[1] : 0,
                      mlen > 2 ? mfr[2] : 0);

        parseMfrData(mfr, mlen);
    }
};

// ── BLE supervisor task ───────────────────────────────────────────────────
// NimBLE is initialised once in setup() (controller buffers stay allocated
// permanently — ~37 KB of internal SRAM is paid up-front). The supervisor
// only orchestrates burst scan + Ultimatron poll cycles:
//
//   1. Active scan 5 s            → Victron Instant Readout
//   2. ultimatronPollOnce()       → BMS GATT poll (~5–10 s)
//   3. Sleep ~20 s                → RF freed for WiFi
//
// Lazy init/deinit was attempted but the C5 BLE controller's HCI bring-up
// fails when called from a non-setup task context after WiFi/AsyncTCP have
// fragmented internal SRAM (largest free block < ~20 KB). Permanent init
// is the pragmatic choice; memory headroom is recovered by trimming LVGL
// pool, task stacks, and WebSocket queue size.
static void bleSupervisorTask(void* /*arg*/) {
    vTaskDelay(pdMS_TO_TICKS(15000));   // let WiFi/MQTT/web settle first
    LOG_BLE_PL("[ble-sup] supervisor task started");

    int      failCount  = 0;
    uint32_t cycleCount = 0;

    // Victron Instant Readout is published at ~1 Hz; the display marks
    // data stale at age >120 s. Run a Victron scan every cycle so a
    // single missed advertisement does not blow the staleness budget.
    // The BMS (SOC, pack voltage, current) changes slowly — poll once
    // every N cycles to amortise its ~5-10 s GATT round-trip.
    constexpr uint32_t ULTIMATRON_EVERY_N_CYCLES = 6;   // ~60 s with 10 s cycle

    for (;;) {
        if (s_bleStopped || s_bleSuspended) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // ── 1. Burst scan for Victron ─────────────────────────────────────
        if (s_configured && s_bleScan) {
            s_bleScan->clearResults();
            // NimBLE 2.x: start(duration_ms, isContinue) returns bool.
            bool ok = s_bleScan->start(5000, false);
            if (!ok) {
                failCount++;
                LOG_BLE_PF("[ble-sup] scan start failed (#%d)\n", failCount);
            } else {
                failCount = 0;
            }
        }

        // ── 2. Ultimatron BMS poll (NimBLE already up) ────────────────────
        // Throttled: only every N cycles. SOC drift is slow, no need to
        // pay the GATT cost every Victron tick.
        if (ultimatronIsConfigured() && (cycleCount % ULTIMATRON_EVERY_N_CYCLES) == 0) {
            ultimatronPollOnce();
        }
        cycleCount++;

        // ── 3. Idle window — RF released to WiFi ──────────────────────────
        // Aggressive: 5 s idle → ~10 s cycle (Victron updates ~10 s)
        // Save     : 30 s idle → ~35 s cycle
        // Cap backoff at 30 s even on repeated scan failures, so a string
        // of misses cannot push data past the 120 s staleness threshold.
        uint32_t idleMs = s_aggressive ? 5000 : 30000;
        if (failCount > 3) idleMs = 30000;
        vTaskDelay(pdMS_TO_TICKS(idleMs));
    }
}

void victronBleSetAggressive(bool aggressive) {
    s_aggressive = aggressive;
}

void victronBleStop() {
    s_bleStopped = true;
    // Do NOT call s_bleScan->stop() here — bleTask may be inside start()
    // and calling stop() from another task corrupts the NimBLE controller.
    // The bleTask checks s_bleStopped and exits its loop gracefully.
}

void victronBleStart() {
    s_bleStopped = false;
}

void victronBleSuspend() {
    s_bleSuspended = true;
    // Do NOT use vTaskSuspend() on bleTask — if it is inside NimBLE APIs
    // the controller ends up in a corrupted state (rc=519 forever).
}

void victronBleResume() {
    s_bleSuspended = false;
}

// ── Public API ────────────────────────────────────────────────────────────
bool victronIsConfigured() {
#ifdef ENABLE_SOLAR_DUMMY
    return true;   // pretend configured so UI shows the dummy values
#else
    return s_configured;
#endif
}

VictronData victronGetData() {
#ifdef ENABLE_SOLAR_DUMMY
    // Dev-mode override: ignore real BLE data and synthesise oscillating
    // values so the UI can be tested without panels/battery nearby.
    float t = millis() / 1000.0f;
    VictronData d = {};
    d.pvW      = 80.0f + 40.0f * sinf(t * 0.8f);
    d.battA    = 3.0f + 2.5f * sinf(t * 0.5f);
    d.battV    = 13.0f + 0.3f * sinf(t * 1.2f);
    d.kWhToday = 1.25f + 0.01f * t;
    d.state    = 3;
    d.errCode  = 0;
    d.valid    = true;
    d.lastMs   = millis();
    return d;
#else
    VictronData copy = {};
    if (s_dataMux) {
        xSemaphoreTake(s_dataMux, portMAX_DELAY);
        copy = s_data;
        xSemaphoreGive(s_dataMux);
    }
    return copy;
#endif
}

void victronBleInit() {
    // Loads NVS config only. NimBLE controller is brought up by bleSupervisorStart().
    String addr, keyHex;
    if (!victronLoadConfig(addr, keyHex)) {
        LOG_BLE_PL("[ble] Victron not configured (no NVS entry)");
        return;
    }
    if (!hexToBytes(keyHex, s_aesKey, 16)) {
        LOG_BLE_PL("[ble] bad encryption key — not 32 valid hex chars");
        return;
    }
    s_targetAddr = addr;
    s_configured = true;
    if (!s_dataMux) s_dataMux = xSemaphoreCreateMutex();
    LOG_BLE_PF("[ble] Victron cfg loaded, target=%s\n", s_targetAddr.c_str());
}

// Bring NimBLE up and start the supervisor task. Call once after both
// victronBleInit() and ultimatronBleInit() have loaded their NVS configs.
// Must be invoked from setup() BEFORE LinBus/LVGL tasks are created — the
// C5 BLE controller can only allocate its ~37 KB of internal SRAM cleanly
// when the heap is not yet fragmented.
void bleSupervisorStart() {
    if (s_bleTaskHandle) return;
    if (!s_configured && !ultimatronIsConfigured()) {
        LOG_BLE_PL("[ble-sup] not started — nothing BLE configured");
        return;
    }

    LOG_BLE_PF("[ble] NimBLE init  int_free=%u int_largest=%u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    NimBLEDevice::init("");
    LOG_BLE_PF("[ble] NimBLE up    int_free=%u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    if (s_configured) {
        s_bleScan = NimBLEDevice::getScan();
        s_bleScan->setScanCallbacks(new VictronScanCb(), /*deleteCallbacks=*/true);
        s_bleScan->setActiveScan(true);   // SCAN_RSP carries Instant Readout
        s_bleScan->setInterval(160);      // 100 ms
        s_bleScan->setWindow(80);         // 50 ms (50 % duty during the 5 s burst)
        s_bleScan->setMaxResults(0);      // fire callback for every adv
    }

    xTaskCreate(bleSupervisorTask, "ble_sup", 4096, nullptr, 1, &s_bleTaskHandle);
    LOG_BLE_PL("[ble-sup] supervisor task created");
}

#else // !ENABLE_BLE — stubs (dummy data only when ENABLE_SOLAR_DUMMY is set)

#ifdef ENABLE_SOLAR_DUMMY
static VictronData s_fakeVictron = {
    13.2f,  5.5f, 120.0f, 1.25f,
    3, 0, true, 0
};
#endif

void victronBleInit() {}
void bleSupervisorStart() {}

VictronData victronGetData() {
#ifdef ENABLE_SOLAR_DUMMY
    float t = millis() / 1000.0f;
    s_fakeVictron.pvW      = 80.0f + 40.0f * sinf(t * 0.8f);
    s_fakeVictron.battA    = 3.0f + 2.5f * sinf(t * 0.5f);
    s_fakeVictron.battV    = 13.0f + 0.3f * sinf(t * 1.2f);
    s_fakeVictron.kWhToday = 1.25f + 0.01f * t;
    s_fakeVictron.lastMs   = millis();
    return s_fakeVictron;
#else
    return VictronData{};   // valid=false → shows "--"
#endif
}

bool victronIsConfigured() { return true; }   // true so UI shows "--" instead of "not configured"
void victronBleSuspend() {}
void victronBleResume() {}
void victronBleSetAggressive(bool) {}
void victronBleStop() {}
void victronBleStart() {}
bool victronLoadConfig(String& addr, String& key) { (void)addr; (void)key; return false; }
void victronSaveConfig(const String& addr, const String& key) { (void)addr; (void)key; }

#endif // ENABLE_BLE
