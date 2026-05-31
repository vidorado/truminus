#include "ultimatronble.hpp"
#include "logs.hpp"
#include <math.h>
#if defined(ENABLE_BLE)
#include <NimBLEDevice.h>
#include "nvs.h"
#include "esp_timer.h"
#include <string.h>
#include <algorithm>
#include <cctype>

static inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

// ── NVS ───────────────────────────────────────────────────────────────────
static const char* NVS_NS   = "batt";
static const char* NVS_ADDR = "addr";
static const char* NVS_PASS = "pass";

bool ultimatronLoadConfig(std::string& addr, std::string& pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char buf[64] = {};
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, NVS_ADDR, buf, &len);
    if (err != ESP_OK) { nvs_close(h); return false; }
    addr = buf;
    len = sizeof(buf); buf[0] = '\0';
    nvs_get_str(h, NVS_PASS, buf, &len);
    pass = buf;
    nvs_close(h);
    return addr.size() == 12;
}

void ultimatronSaveConfig(const std::string& addr, const std::string& pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_ADDR, addr.c_str());
    nvs_set_str(h, NVS_PASS, pass.c_str());
    nvs_commit(h);
    nvs_close(h);
}

// ── Module state ──────────────────────────────────────────────────────────
static bool          s_configured   = false;
static std::string   s_targetAddr;
static std::string   s_password;

static SemaphoreHandle_t s_dataMux  = nullptr;
static SemaphoreHandle_t s_rxSem    = nullptr;
static UltimatronData    s_data     = {};

static volatile bool s_ultSuspended = false;

// DIAGNOSTIC: last time the BMS MAC was seen in a scan window (0 = never).
static volatile uint32_t s_lastSeenMs = 0;
static volatile bool     s_everSeen   = false;

static uint8_t  s_rxBuf[64] = {};
static int      s_rxLen     = 0;

// ── Notification callback ─────────────────────────────────────────────────
static void notifyCb(NimBLERemoteCharacteristic* /*ch*/,
                     uint8_t* data, size_t len, bool /*isNotify*/) {
    int space = (int)sizeof(s_rxBuf) - s_rxLen;
    int copy  = std::min((int)len, space);
    if (copy > 0) {
        memcpy(s_rxBuf + s_rxLen, data, copy);
        s_rxLen += copy;
    }
    if (s_rxLen >= 4 && s_rxBuf[0] == 0xdd && s_rxBuf[1] == 0x03 && s_rxBuf[2] == 0x00) {
        int expected = 4 + (int)s_rxBuf[3] + 3;
        if (s_rxLen >= expected && s_rxSem)
            xSemaphoreGive(s_rxSem);
    }
}

// ── Format "AABBCCDDEEFF" → "AA:BB:CC:DD:EE:FF" ──────────────────────────
static std::string formatMac(const std::string& compact) {
    if (compact.size() != 12) return compact;
    std::string s;
    for (int i = 0; i < 12; i += 2) {
        if (i > 0) s += ':';
        s += compact[i];
        s += compact[i + 1];
    }
    return s;
}

// ── Parse response packet ─────────────────────────────────────────────────
static void parseResponse(const uint8_t* d, int len) {
    if (len < 27 || d[0] != 0xdd || d[1] != 0x03 || d[2] != 0x00) return;
    uint16_t rawV = ((uint16_t)d[4] << 8) | d[5];
    int16_t  rawA = (int16_t)(((uint16_t)d[6] << 8) | d[7]);
    UltimatronData ud = {};
    ud.battV  = rawV / 100.0f;
    ud.battA  = rawA / 100.0f;
    ud.soc    = d[23];
    ud.valid  = true;
    ud.lastMs = millis();
    if (len >= 30) {
        int16_t rawT = (int16_t)(((uint16_t)d[27] << 8) | d[28]);
        ud.tempC = (rawT - 2731) / 10.0f;
    }
    LOG_ULT_PF("[ult] SOC=%u%% V=%.1fV A=%.2fA T=%.1f°C\n",
               ud.soc, ud.battV, ud.battA, ud.tempC);
    xSemaphoreTake(s_dataMux, portMAX_DELAY);
    s_data = ud;
    xSemaphoreGive(s_dataMux);
}

// ── Single poll ───────────────────────────────────────────────────────────
bool ultimatronPollOnce() {
    if (!s_configured || s_ultSuspended) return false;

    uint32_t t0 = millis();
    std::string macStr = formatMac(s_targetAddr);
    NimBLEAddress addr(macStr, 0);

    NimBLEClient* client = NimBLEDevice::createClient();
    client->setConnectTimeout(5000);

    bool connected = false;
    for (int attempt = 1; attempt <= 3 && !connected; attempt++) {
        connected = client->connect(addr);
        if (!connected && attempt < 3) vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!connected) {
        LOG_ULT_PF("[ult] connect failed (+%ums)\n", (unsigned)(millis() - t0));
        NimBLEDevice::deleteClient(client);
        return false;
    }

    NimBLERemoteService* svc = client->getService("ff00");
    if (!svc) {
        client->disconnect(); NimBLEDevice::deleteClient(client); return false;
    }
    NimBLERemoteCharacteristic* notifChar = svc->getCharacteristic("ff01");
    NimBLERemoteCharacteristic* writeChar = svc->getCharacteristic("ff02");
    if (!notifChar || !writeChar) {
        client->disconnect(); NimBLEDevice::deleteClient(client); return false;
    }
    if (!notifChar->subscribe(true, notifyCb)) {
        client->disconnect(); NimBLEDevice::deleteClient(client); return false;
    }

    // Optional password authentication
    if (s_password.size() == 6) {
        uint8_t auth[13];
        auth[0] = 0xDD; auth[1] = 0x5A; auth[2] = 0x60; auth[3] = 0x06;
        for (int i = 0; i < 6; i++) auth[4 + i] = (uint8_t)s_password[i];
        uint16_t sum = 0;
        for (int i = 2; i < 10; i++) sum += auth[i];
        uint16_t chk = (uint16_t)(0x10000u - sum);
        auth[10] = (uint8_t)(chk >> 8);
        auth[11] = (uint8_t)(chk & 0xFF);
        auth[12] = 0x77;
        s_rxLen = 0; memset(s_rxBuf, 0, sizeof(s_rxBuf));
        while (xSemaphoreTake(s_rxSem, 0) == pdTRUE) {}
        writeChar->writeValue(auth, sizeof(auth), false);
        xSemaphoreTake(s_rxSem, pdMS_TO_TICKS(500));
    }

    const uint8_t cmd[] = {0xDD, 0xA5, 0x03, 0x00, 0xFF, 0xFD, 0x77};
    bool got = false;
    for (int attempt = 1; attempt <= 5 && !got; attempt++) {
        s_rxLen = 0; memset(s_rxBuf, 0, sizeof(s_rxBuf));
        while (xSemaphoreTake(s_rxSem, 0) == pdTRUE) {}
        writeChar->writeValue(cmd, sizeof(cmd), false);
        got = (xSemaphoreTake(s_rxSem, pdMS_TO_TICKS(2000)) == pdTRUE);
    }

    client->disconnect();
    NimBLEDevice::deleteClient(client);
    if (got) parseResponse(s_rxBuf, s_rxLen);
    LOG_ULT_PF("[ult] poll %s (+%ums)\n", got ? "ok" : "failed", (unsigned)(millis() - t0));
    return got;
}

// ── Advertisement observer (diagnostic) ───────────────────────────────────
void ultimatronBleHandleAd(const NimBLEAdvertisedDevice* dev) {
    if (!s_configured || !dev) return;
    std::string devAddr;
    for (char c : dev->getAddress().toString())
        if (c != ':') devAddr += (char)toupper((unsigned char)c);
    if (devAddr != s_targetAddr) return;
    s_lastSeenMs = millis();
    if (!s_everSeen) {
        s_everSeen = true;
        LOG_ULT_PF("[ult] advert seen, target present rssi=%d\n", dev->getRSSI());
    }
}

uint32_t ultimatronLastSeenMs() { return s_lastSeenMs; }

// ── Public API ────────────────────────────────────────────────────────────
bool ultimatronIsConfigured() {
#ifdef ENABLE_SOLAR_DUMMY
    return true;
#else
    return s_configured;
#endif
}

static void load_config_internal() {
    std::string addr, pass;
    if (!ultimatronLoadConfig(addr, pass)) {
        s_configured = false;
        return;
    }
    // Normalise to uppercase, no separators, so the advert-observer's MAC
    // compare (which uppercases the scanned address) matches regardless of how
    // the address was stored.  NimBLEAddress/formatMac are case-insensitive, so
    // the GATT connect path is unaffected.
    s_targetAddr.clear();
    for (char c : addr)
        if (c != ':') s_targetAddr += (char)toupper((unsigned char)c);
    s_password    = pass;
    s_configured  = true;
    if (!s_dataMux) s_dataMux = xSemaphoreCreateMutex();
    if (!s_rxSem)   s_rxSem   = xSemaphoreCreateBinary();
}

void ultimatronBleInit() {
    load_config_internal();
    if (s_configured)
        LOG_ULT_PF("[ult] cfg loaded, target=%s pass=%s\n",
                   s_targetAddr.c_str(), s_password.size() ? "yes" : "no");
    else
        LOG_ULT_PL("[ult] not configured");
}

void ultimatronBleReloadConfig() {
    load_config_internal();
    if (s_configured)
        LOG_ULT_PF("[ult] cfg reloaded, target=%s\n", s_targetAddr.c_str());
}

UltimatronData ultimatronGetData() {
#ifdef ENABLE_SOLAR_DUMMY
    float t = (float)(esp_timer_get_time() / 1000000ULL);
    UltimatronData d = {};
    d.soc   = (uint8_t)(60 + (int)(20.0f * sinf(t * 0.6f)));
    d.battA = -1.5f + 1.0f * sinf(t * 0.9f);
    d.battV = 13.0f + 0.2f * sinf(t * 1.1f);
    d.tempC = 18.0f + 3.0f * sinf(t * 0.4f);
    d.valid = true; d.lastMs = millis();
    return d;
#else
    UltimatronData copy = {};
    if (s_dataMux) {
        xSemaphoreTake(s_dataMux, portMAX_DELAY);
        copy = s_data;
        xSemaphoreGive(s_dataMux);
    }
    return copy;
#endif
}

void ultimatronBleSuspend() { s_ultSuspended = true;  }
void ultimatronBleResume()  { s_ultSuspended = false; }

#else // !ENABLE_BLE

void ultimatronBleInit() {}
void ultimatronBleReloadConfig() {}

UltimatronData ultimatronGetData() {
#ifdef ENABLE_SOLAR_DUMMY
    static UltimatronData d = { 78, 13.1f, -2.3f, 18.5f, true, 0 };
    float t = (float)(esp_timer_get_time() / 1000000ULL);
    d.soc     = (uint8_t)(60 + (int)(20.0f * sinf(t * 0.6f)));
    d.battA   = -1.5f + 1.0f * sinf(t * 0.9f);
    d.battV   = 13.0f + 0.2f * sinf(t * 1.1f);
    d.tempC   = 18.0f + 3.0f * sinf(t * 0.4f);
    return d;
#else
    return UltimatronData{};
#endif
}

bool ultimatronIsConfigured() { return false; }
bool ultimatronPollOnce()     { return false; }
void ultimatronBleHandleAd(const NimBLEAdvertisedDevice*) {}
uint32_t ultimatronLastSeenMs() { return 0; }
void ultimatronBleSuspend()   {}
void ultimatronBleResume()    {}
bool ultimatronLoadConfig(std::string& addr, std::string& pass) { (void)addr; (void)pass; return false; }
void ultimatronSaveConfig(const std::string& addr, const std::string& pass) { (void)addr; (void)pass; }

#endif // ENABLE_BLE
