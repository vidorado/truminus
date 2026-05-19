#include "victronble.hpp"
#include "ultimatronble.hpp"
#include "logs.hpp"
#include <math.h>
#if defined(ENABLE_BLE)
#include <NimBLEDevice.h>
#include "aes/esp_aes.h"
#include <esp_heap_caps.h>
#include "nvs.h"
#include "esp_timer.h"
#include <string.h>
#include <algorithm>

static inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

// ── NVS ───────────────────────────────────────────────────────────────────
static const char* NVS_NS   = "solar";
static const char* NVS_ADDR = "addr";
static const char* NVS_KEY  = "key";

bool victronLoadConfig(std::string& addr, std::string& key) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char buf[64] = {};
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, NVS_ADDR, buf, &len);
    if (err != ESP_OK) { nvs_close(h); return false; }
    addr = buf;
    len = sizeof(buf); buf[0] = '\0';
    nvs_get_str(h, NVS_KEY, buf, &len);
    key = buf;
    nvs_close(h);
    return addr.size() == 12 && key.size() == 32;
}

void victronSaveConfig(const std::string& addr, const std::string& key) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_ADDR, addr.c_str());
    nvs_set_str(h, NVS_KEY,  key.c_str());
    nvs_commit(h);
    nvs_close(h);
}

// ── Module state ──────────────────────────────────────────────────────────
static bool          s_configured      = false;
static uint8_t       s_aesKey[16]      = {};
static std::string   s_targetAddr;

static SemaphoreHandle_t s_dataMux      = nullptr;
static VictronData       s_data         = {};

static NimBLEScan*   s_bleScan          = nullptr;
static TaskHandle_t  s_bleTaskHandle    = nullptr;
static volatile bool s_aggressive       = true;
static volatile bool s_bleStopped       = false;
static volatile bool s_bleSuspended     = false;
static volatile bool s_supervisorInScan = false;
static volatile bool s_nimbleUp         = false;
static volatile bool s_discoveryRunning = false;

// ── Hex helpers ───────────────────────────────────────────────────────────
static bool hexToBytes(const std::string& hex, uint8_t* out, int len) {
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

static std::string normaliseAddr(const std::string& raw) {
    std::string s;
    for (char c : raw) {
        if (c != ':') s += (char)toupper((unsigned char)c);
    }
    return s;
}

// ── AES-128-CTR decrypt ───────────────────────────────────────────────────
static bool aesCtrDecrypt(const uint8_t* cipher, int len,
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
    int ret = esp_aes_crypt_ctr(&ctx, (size_t)len, &nc_off,
                                nonce, stream_block, cipher, out);
    esp_aes_free(&ctx);
    return ret == 0;
}

// ── Parse Victron Instant Readout advertisement ───────────────────────────
static void parseMfrData(const uint8_t* mfr, int len) {
    if (len < 18) return;
    if (mfr[0] != 0xE1 || mfr[1] != 0x02) return;
    if (mfr[2] != 0x10) return;
    if (mfr[9] != s_aesKey[0]) return;

    uint8_t out[16] = {};
    if (!aesCtrDecrypt(mfr + 10, 16, mfr[7], mfr[8], out)) return;

    int16_t  rawV   = (int16_t)((uint16_t)out[2] | ((uint16_t)out[3] << 8));
    int16_t  rawA   = (int16_t)((uint16_t)out[4] | ((uint16_t)out[5] << 8));
    uint16_t rawKwh = (uint16_t)out[6] | ((uint16_t)out[7] << 8);
    uint16_t rawPv  = (uint16_t)out[8] | ((uint16_t)out[9] << 8);

    if (rawV == 0x7FFF) return;

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

// ── Victron scan callback (continuous monitoring) ─────────────────────────
class VictronScanCb : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!dev->haveManufacturerData()) return;
        if (s_targetAddr.size() > 0) {
            std::string devAddr = normaliseAddr(dev->getAddress().toString());
            if (devAddr != s_targetAddr) return;
        }
        std::string raw = dev->getManufacturerData();
        parseMfrData((const uint8_t*)raw.data(), (int)raw.size());
    }
};

// ── Discovery scan callback (one-shot for settings UI) ────────────────────
#define MAX_DISCOVERED 24
static BleDevice s_disc[MAX_DISCOVERED];
static int       s_discCount = 0;

class DiscoveryScanCb : public NimBLEScanCallbacks {
    bool _victronOnly;
public:
    explicit DiscoveryScanCb(bool victronOnly) : _victronOnly(victronOnly) {}
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (s_discCount >= MAX_DISCOVERED) return;
        std::string mac = normaliseAddr(dev->getAddress().toString());
        // Deduplicate
        for (int i = 0; i < s_discCount; i++) {
            if (mac == s_disc[i].mac) return;
        }
        bool isVictron = false;
        if (dev->haveManufacturerData()) {
            const std::string& mfr = dev->getManufacturerData();
            if (mfr.size() >= 2 &&
                (uint8_t)mfr[0] == 0xE1 && (uint8_t)mfr[1] == 0x02) {
                isVictron = true;
            }
        }
        if (_victronOnly && !isVictron) return;

        BleDevice& d = s_disc[s_discCount++];
        if (dev->haveName() && dev->getName().size() > 0) {
            strncpy(d.name, dev->getName().c_str(), sizeof(d.name) - 1);
        } else {
            strncpy(d.name, dev->getAddress().toString().c_str(), sizeof(d.name) - 1);
        }
        d.name[sizeof(d.name) - 1] = '\0';
        strncpy(d.mac, mac.c_str(), sizeof(d.mac) - 1);
        d.mac[sizeof(d.mac) - 1] = '\0';
        d.is_victron = isVictron;
    }
};

struct DiscoveryScanArgs {
    BleDiscoveryCb cb;
    void*          user;
    uint32_t       duration_ms;
    bool           victron_only;
};

static void discovery_scan_task(void* arg) {
    auto* a = (DiscoveryScanArgs*)arg;
    BleDiscoveryCb cb          = a->cb;
    void*          user        = a->user;
    uint32_t       duration_ms = a->duration_ms;
    bool           vo          = a->victron_only;
    free(a);

    // Wait for any in-progress supervisor scan to finish (max 6 s).
    for (int i = 0; i < 60 && s_supervisorInScan; i++) vTaskDelay(pdMS_TO_TICKS(100));

    // Init NimBLE if not already up.
    if (!s_nimbleUp) {
        NimBLEDevice::init("");
        s_nimbleUp = true;
    }

    s_discCount = 0;
    memset(s_disc, 0, sizeof(s_disc));

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(new DiscoveryScanCb(vo), true);
    scan->setActiveScan(true);
    scan->setInterval(160);
    scan->setWindow(80);
    scan->setMaxResults(0);
    scan->clearResults();
    // NimBLE 2.x: start() is non-blocking and returns as soon as the GAP
    // procedure is queued. We need to wait for the duration window (during
    // which DiscoveryScanCb::onResult fills s_disc) before reporting back.
    if (scan->start(duration_ms, false)) {
        uint32_t waited = 0;
        while (scan->isScanning() && waited < duration_ms + 1000) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
        if (scan->isScanning()) scan->stop();
    }

    // Restore supervisor's scan callback (if configured).
    if (s_configured && s_bleScan) {
        scan->setScanCallbacks(new VictronScanCb(), true);
    } else {
        scan->setScanCallbacks(nullptr, false);
    }

    cb(s_disc, s_discCount, user);
    s_discoveryRunning = false;
    vTaskDelete(nullptr);
}

void bleDiscoveryScan(bool victron_only, BleDiscoveryCb cb, void* user, uint32_t duration_ms) {
    if (s_discoveryRunning) { cb(nullptr, 0, user); return; }
    s_discoveryRunning = true;

    auto* a           = (DiscoveryScanArgs*)malloc(sizeof(DiscoveryScanArgs));
    a->cb             = cb;
    a->user           = user;
    a->duration_ms    = duration_ms;
    a->victron_only   = victron_only;
    xTaskCreate(discovery_scan_task, "ble_disc", 8192, a, 2, nullptr);
}

// ── BLE supervisor task ───────────────────────────────────────────────────
static void bleSupervisorTask(void* /*arg*/) {
    vTaskDelay(pdMS_TO_TICKS(15000));
    LOG_BLE_PL("[ble-sup] started");

    int      failCount  = 0;
    uint32_t cycleCount = 0;
    constexpr uint32_t ULTIMATRON_EVERY_N = 6;

    for (;;) {
        if (s_bleStopped || s_bleSuspended || s_discoveryRunning) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (s_configured && s_bleScan) {
            s_bleScan->clearResults();
            s_supervisorInScan = true;
            bool ok = s_bleScan->start(5000, false);
            s_supervisorInScan = false;
            if (!ok) failCount++;
            else     failCount = 0;
        }

        if (ultimatronIsConfigured() && (cycleCount % ULTIMATRON_EVERY_N) == 0) {
            ultimatronPollOnce();
        }
        cycleCount++;

        uint32_t idleMs = s_aggressive ? 5000 : 30000;
        if (failCount > 3) idleMs = 30000;
        vTaskDelay(pdMS_TO_TICKS(idleMs));
    }
}

// ── Public API ────────────────────────────────────────────────────────────
void victronBleSetAggressive(bool aggressive) { s_aggressive = aggressive; }
void victronBleStop()    { s_bleStopped    = true;  }
void victronBleStart()   { s_bleStopped    = false; }
void victronBleSuspend() { s_bleSuspended  = true;  }
void victronBleResume()  { s_bleSuspended  = false; }

bool victronIsConfigured() {
#ifdef ENABLE_SOLAR_DUMMY
    return true;
#else
    return s_configured;
#endif
}

static void load_config_internal() {
    std::string addr, keyHex;
    if (!victronLoadConfig(addr, keyHex)) {
        s_configured = false;
        return;
    }
    if (!hexToBytes(keyHex, s_aesKey, 16)) {
        s_configured = false;
        return;
    }
    s_targetAddr  = addr;
    s_configured  = true;
    if (!s_dataMux) s_dataMux = xSemaphoreCreateMutex();
}

void victronBleInit() {
    load_config_internal();
    if (s_configured)
        LOG_BLE_PF("[ble] Victron cfg loaded, target=%s\n", s_targetAddr.c_str());
    else
        LOG_BLE_PL("[ble] Victron not configured");
}

void victronBleReloadConfig() {
    load_config_internal();
    if (s_configured)
        LOG_BLE_PF("[ble] Victron cfg reloaded, target=%s\n", s_targetAddr.c_str());
}

VictronData victronGetData() {
#ifdef ENABLE_SOLAR_DUMMY
    float t = millis() / 1000.0f;
    VictronData d = {};
    d.pvW      = 80.0f  + 40.0f * sinf(t * 0.8f);
    d.battA    = 3.0f   + 2.5f  * sinf(t * 0.5f);
    d.battV    = 13.0f  + 0.3f  * sinf(t * 1.2f);
    d.kWhToday = 1.25f  + 0.01f * t;
    d.state    = 3; d.errCode = 0; d.valid = true; d.lastMs = millis();
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

void bleSupervisorStart() {
    if (s_bleTaskHandle) return;
    if (!s_configured && !ultimatronIsConfigured()) {
        LOG_BLE_PL("[ble-sup] nothing configured — NimBLE still inited for discovery");
    }

    LOG_BLE_PF("[ble] NimBLE init  free=%u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    NimBLEDevice::init("");
    s_nimbleUp = true;
    LOG_BLE_PF("[ble] NimBLE up    free=%u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    if (s_configured) {
        s_bleScan = NimBLEDevice::getScan();
        s_bleScan->setScanCallbacks(new VictronScanCb(), true);
        s_bleScan->setActiveScan(true);
        s_bleScan->setInterval(160);
        s_bleScan->setWindow(80);
        s_bleScan->setMaxResults(0);
    }

    xTaskCreate(bleSupervisorTask, "ble_sup", 4096, nullptr, 1, &s_bleTaskHandle);
    LOG_BLE_PL("[ble-sup] task created");
}

#else // !ENABLE_BLE

void victronBleInit() {}
void bleSupervisorStart() {}
void victronBleReloadConfig() {}

VictronData victronGetData() {
#ifdef ENABLE_SOLAR_DUMMY
    static VictronData d = { 13.2f, 5.5f, 120.0f, 1.25f, 3, 0, true, 0 };
    float t = (float)(esp_timer_get_time() / 1000000ULL);
    d.pvW   = 80.0f + 40.0f * sinf(t * 0.8f);
    d.battA = 3.0f  + 2.5f  * sinf(t * 0.5f);
    d.battV = 13.0f + 0.3f  * sinf(t * 1.2f);
    return d;
#else
    return VictronData{};
#endif
}

bool victronIsConfigured() { return false; }
void victronBleSuspend()   {}
void victronBleResume()    {}
void victronBleSetAggressive(bool) {}
void victronBleStop()      {}
void victronBleStart()     {}
bool victronLoadConfig(std::string& addr, std::string& key) { (void)addr; (void)key; return false; }
void victronSaveConfig(const std::string& addr, const std::string& key) { (void)addr; (void)key; }
void bleDiscoveryScan(bool, BleDiscoveryCb cb, void* user, uint32_t) { cb(nullptr, 0, user); }

#endif // ENABLE_BLE
