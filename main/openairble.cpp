// OpenAir PLUS A/C (Bergstrom/Dirna) — BLE GATT client driver.
//
// Protocol reference: .claude/skills/openair-plus/SKILL.md
//
// Transport: BLE GATT central (NimBLE 2.x), active connection per poll.
//
// Service:     e43ff2c2-8602-48f6-82d0-72cd56fb06f2
// Handshake:   9d667ea8-9c95-4dd0-b952-92031c4f5375  (WRITE once, before subscribing)
// Data char:   4a01b4dd-350d-4afc-9a9f-27164f2b6b56  (NOTIFY + READ + WRITE)
//
// Wire format: ASCII-hex text, big-endian per field.
//   Write (app→device): 30 bytes → 60 ASCII chars (fields 0–12)
//   Read  (device→app): 60 bytes → 120 chars used (+ 4 chars discarded = 124 total)
//
// Byte map of the write frame:
//   0-3   RealTimeClock  u32  seconds since midnight (big-endian, 8 hex chars)
//   4-5   BatteryType         always 0
//   6-7   Power               always 1
//   8-9   TempScale           0 = °C
//   10-11 PowerState          0=OFF 1=ON
//   12-13 Mode                0=AUTO 2=MAN (1=ECO unconfirmed)
//   14-15 Temp                tenths of °C  (210 = 21.0 °C)
//   16-17 BlowerSpeed         1–6
//   18-19 LedBright           0/1
//   20-21 (reserved)          0
//   22-23 LedColor            colour index
//   24-25 ScheduledTime       timer value
//   26-27 Flaps1Mode          0/1
//   28-29 Flaps2Mode          0/1
//
// Connection model: passive scan (MAC match) → detect device → connect →
//   handshake → subscribe → write cmd → wait notify → parse telemetry →
//   disconnect.  Driven by bleSupervisorTask every scan cycle.

#include "openairble.hpp"
#include "logs.hpp"
#include <cstring>
#include <cctype>
#include <cstdio>
#include <algorithm>

#if defined(ENABLE_BLE)
#include <NimBLEDevice.h>
#include "nvs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "openair";
static inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

// ── GATT identifiers ────────────────────────────────────────────────────────
static const char* SVC_UUID       = "e43ff2c2-8602-48f6-82d0-72cd56fb06f2";
static const char* HANDSHAKE_UUID = "9d667ea8-9c95-4dd0-b952-92031c4f5375";
static const char* DATA_UUID      = "4a01b4dd-350d-4afc-9a9f-27164f2b6b56";

// ── NVS ─────────────────────────────────────────────────────────────────────
static const char* NVS_NS   = "openair";
static const char* NVS_ADDR = "addr";

// ── Module state ─────────────────────────────────────────────────────────────
static bool              s_configured  = false;
static std::string       s_targetAddr;        // uppercase hex, 12 chars, no colons

static SemaphoreHandle_t s_dataMux     = nullptr;
static SemaphoreHandle_t s_cmdMux      = nullptr;
static SemaphoreHandle_t s_notifySem   = nullptr;
static OpenAirData       s_data        = {};
static OpenAirCmd        s_pendingCmd  = {};

static volatile uint32_t s_lastSeenMs  = 0;

// Service-data bytes captured from the peripheral's ADV_IND, used in the
// handshake write.  Captured once; stays zeroed if the peripheral doesn't
// include service-data in its passive-scan ADV_IND.
static uint8_t s_advSvcData[16]        = {};
static int     s_advSvcDataLen         = 0;

// 4-byte TruMinus identifier appended to service-data in the handshake.
// The official app uses a hash of the Android device-id; we use a fixed tag.
static const uint8_t TRUMINUS_ID[4]   = {0x54, 0x52, 0x4D, 0x4E}; // "TRMN"

// Notification receive buffer — written by the NimBLE host task, read from
// the supervisor task after the semaphore handshake.
static uint8_t s_notifyBuf[128]        = {};
static int     s_notifyLen             = 0;

// ── Notification callback ────────────────────────────────────────────────────
static void notifyCb(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    if (len > sizeof(s_notifyBuf)) len = sizeof(s_notifyBuf);
    memcpy(s_notifyBuf, data, len);
    s_notifyLen = (int)len;
    if (s_notifySem) xSemaphoreGive(s_notifySem);
}

// ── Helpers ─────────────────────────────────────────────────────────────────
static int hexNibble(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint16_t readU16BE(const uint8_t* b, int off) {
    return ((uint16_t)b[off] << 8) | b[off + 1];
}

// Format compact MAC "AABBCCDDEEFF" → "AA:BB:CC:DD:EE:FF"
static std::string formatMac(const std::string& compact) {
    if (compact.size() != 12) return compact;
    std::string s;
    for (int i = 0; i < 12; i += 2) {
        if (i > 0) s += ':';
        s += compact[i]; s += compact[i + 1];
    }
    return s;
}

// ── Telemetry parser ─────────────────────────────────────────────────────────
// Parses the 124-char (min 120) ASCII-hex notification from the device.
// The first 120 chars decode to 60 bytes; the last 4 chars are discarded.
// Read-only fields start at byte 30 (logical field 13 in the Mensaje model).
static bool parseNotification(const uint8_t* ascii, int len) {
    if (len < 120) {
        ESP_LOGW(TAG, "notify too short: %d chars", len);
        return false;
    }
    uint8_t bytes[60] = {};
    for (int i = 0; i < 60; i++) {
        int hi = hexNibble(ascii[i * 2]);
        int lo = hexNibble(ascii[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            ESP_LOGW(TAG, "bad hex at byte %d", i);
            return false;
        }
        bytes[i] = (uint8_t)((hi << 4) | lo);
    }

    OpenAirData d = {};
    d.errors             = (int)readU16BE(bytes, 30);  // field 13
    d.batteryValue       = (int)readU16BE(bytes, 32);  // field 14
    // field 15 (ElectroSpeed)   at bytes 34-35 — not stored yet
    // field 16 (CompressorSpeed) at bytes 36-37 — not stored yet
    d.blowerSpeedPct     = (int)readU16BE(bytes, 38);  // field 17
    // field 18 (ElectroSpeedPct) at bytes 40-41 — not stored yet
    d.compressorSpeedRpm = (int)readU16BE(bytes, 42);  // field 19
    // fields 20-21 (TiltX/Y)    at bytes 44-47 — not stored
    // field 22 (Sonda1F)         at bytes 48-49 — Fahrenheit, skip
    d.probe1C = (float)(int16_t)readU16BE(bytes, 50);  // field 23 Sonda1C
    // field 24 (Sonda2F)         at bytes 52-53 — Fahrenheit, skip
    d.probe2C = (float)(int16_t)readU16BE(bytes, 54);  // field 25 Sonda2C
    d.valid   = true;
    d.lastMs  = millis();

    ESP_LOGI(TAG, "telemetry probe1=%.0f probe2=%.0f blower=%d%% comp_rpm=%d err=%d",
             d.probe1C, d.probe2C, d.blowerSpeedPct, d.compressorSpeedRpm, d.errors);

    xSemaphoreTake(s_dataMux, portMAX_DELAY);
    s_data = d;
    xSemaphoreGive(s_dataMux);
    return true;
}

// ── Command frame builder ─────────────────────────────────────────────────────
// Encodes the 30-byte command as 60 ASCII lowercase hex chars (big-endian per
// field).  RealTimeClock is seconds since boot mod 86400 (SNTP not required).
static void buildWriteFrame(const OpenAirCmd& cmd, char out[61]) {
    uint32_t rtc = (uint32_t)((esp_timer_get_time() / 1000000ULL) % 86400UL);
    snprintf(out, 61,
        "%08x"  // [0-3]   RealTimeClock (u32)
        "%04x"  // [4-5]   BatteryType = 0
        "%04x"  // [6-7]   Power = 1
        "%04x"  // [8-9]   TempScale = 0 (°C)
        "%04x"  // [10-11] PowerState
        "%04x"  // [12-13] Mode
        "%04x"  // [14-15] Temp (tenths °C)
        "%04x"  // [16-17] BlowerSpeed
        "%04x"  // [18-19] LedBright
        "%04x"  // [20-21] reserved
        "%04x"  // [22-23] LedColor
        "%04x"  // [24-25] ScheduledTime
        "%04x"  // [26-27] Flaps1Mode
        "%04x", // [28-29] Flaps2Mode
        (unsigned)rtc,
        0u,
        1u,
        0u,
        (unsigned)cmd.powerState,
        (unsigned)cmd.mode,
        (unsigned)cmd.tempTenths,
        (unsigned)cmd.blowerSpeed,
        (unsigned)cmd.ledBright,
        0u,
        (unsigned)cmd.ledColor,
        (unsigned)cmd.scheduledTime,
        (unsigned)cmd.flaps1,
        (unsigned)cmd.flaps2);
}

// ── Public API ───────────────────────────────────────────────────────────────

bool openairPollOnce() {
    if (!s_configured) return false;

    uint32_t t0 = millis();
    std::string macStr = formatMac(s_targetAddr);
    NimBLEAddress addr(macStr, 0);

    NimBLEClient* client = NimBLEDevice::createClient();
    client->setConnectTimeout(5000);

    ESP_LOGI(TAG, "connecting %s", macStr.c_str());
    bool connected = false;
    for (int attempt = 1; attempt <= 3 && !connected; attempt++) {
        connected = client->connect(addr);
        if (!connected && attempt < 3) vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!connected) {
        ESP_LOGW(TAG, "connect failed (+%ums)", (unsigned)(millis() - t0));
        NimBLEDevice::deleteClient(client);
        return false;
    }
    ESP_LOGI(TAG, "connected (+%ums)", (unsigned)(millis() - t0));

    // Force a full GATT attribute discovery before querying services.
    // Without this, getService() triggers a per-UUID filtered discovery that
    // sometimes fails on 128-bit custom UUIDs even when the service is present.
    if (!client->discoverAttributes()) {
        ESP_LOGW(TAG, "attribute discovery failed");
        client->disconnect(); NimBLEDevice::deleteClient(client); return false;
    }

    NimBLERemoteService* svc = client->getService(SVC_UUID);
    if (!svc) {
        // Dump all discovered services so we can diagnose UUID mismatches.
        ESP_LOGW(TAG, "target service not found; services present:");
        for (auto* s : client->getServices()) {
            ESP_LOGW(TAG, "  svc: %s", s->getUUID().toString().c_str());
        }
        client->disconnect(); NimBLEDevice::deleteClient(client); return false;
    }

    NimBLERemoteCharacteristic* hsChar   = svc->getCharacteristic(HANDSHAKE_UUID);
    NimBLERemoteCharacteristic* dataChar = svc->getCharacteristic(DATA_UUID);
    if (!hsChar || !dataChar) {
        ESP_LOGW(TAG, "characteristics not found");
        client->disconnect(); NimBLEDevice::deleteClient(client); return false;
    }

    // Handshake: [captured service-data bytes (0-12 bytes)] + [4-byte TruMinus ID]
    {
        uint8_t hsBuf[20];
        int hsLen = 0;
        if (s_advSvcDataLen > 0 && s_advSvcDataLen <= (int)(sizeof(hsBuf) - 4)) {
            memcpy(hsBuf, s_advSvcData, (size_t)s_advSvcDataLen);
            hsLen = s_advSvcDataLen;
        }
        memcpy(hsBuf + hsLen, TRUMINUS_ID, 4);
        hsLen += 4;
        hsChar->writeValue(hsBuf, (size_t)hsLen, false);
        ESP_LOGI(TAG, "handshake written (%d bytes)", hsLen);
    }
    // The official app waits ~2 s after the handshake before subscribing.
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Enable notifications on the data characteristic.
    if (!dataChar->subscribe(true, notifyCb)) {
        ESP_LOGW(TAG, "subscribe failed");
        client->disconnect(); NimBLEDevice::deleteClient(client); return false;
    }

    // Snapshot the latest pending command under its mutex.
    OpenAirCmd cmd = {};
    xSemaphoreTake(s_cmdMux, portMAX_DELAY);
    cmd = s_pendingCmd;
    xSemaphoreGive(s_cmdMux);

    // Build the 60-char ASCII-hex command frame and write it.
    // The wire value IS the ASCII code units ("0000..."), not raw binary.
    char frame[61] = {};
    buildWriteFrame(cmd, frame);
    dataChar->writeValue((const uint8_t*)frame, 60, false);
    ESP_LOGI(TAG, "cmd: ps=%d mode=%d temp=%.1f°C fan=%d",
             cmd.powerState, cmd.mode, cmd.tempTenths / 10.0f, cmd.blowerSpeed);

    // Wait for the device to push back a telemetry notification.
    while (xSemaphoreTake(s_notifySem, 0) == pdTRUE) {}   // drain stale
    bool got = (xSemaphoreTake(s_notifySem, pdMS_TO_TICKS(5000)) == pdTRUE);
    if (got) parseNotification(s_notifyBuf, s_notifyLen);
    else     ESP_LOGW(TAG, "no notification within 5 s");

    client->disconnect();
    NimBLEDevice::deleteClient(client);
    ESP_LOGI(TAG, "poll %s (+%ums)", got ? "ok" : "no-notify", (unsigned)(millis() - t0));
    return got;
}

void openairBleHandleAd(const NimBLEAdvertisedDevice* dev) {
    if (!s_configured || !dev) return;

    // Match by configured MAC (normalised, no colons, uppercase).
    std::string devAddr;
    for (char c : dev->getAddress().toString())
        if (c != ':') devAddr += (char)toupper((unsigned char)c);
    if (devAddr != s_targetAddr) return;

    s_lastSeenMs = millis();

    // Capture service-data bytes once for use in the handshake write.
    if (s_advSvcDataLen == 0 && dev->haveServiceData()) {
        std::string sd = dev->getServiceData();
        int len = (int)sd.size();
        if (len > (int)sizeof(s_advSvcData)) len = (int)sizeof(s_advSvcData);
        if (len > 0) {
            memcpy(s_advSvcData, sd.data(), (size_t)len);
            s_advSvcDataLen = len;
            ESP_LOGI(TAG, "captured %d service-data bytes from adv", len);
        }
    }
}

uint32_t openairLastSeenMs() { return s_lastSeenMs; }

bool openairIsConfigured() { return s_configured; }

OpenAirData openairGetData() {
    OpenAirData copy = {};
    if (s_dataMux) {
        xSemaphoreTake(s_dataMux, portMAX_DELAY);
        copy = s_data;
        xSemaphoreGive(s_dataMux);
    }
    return copy;
}

void openairSetCmd(const OpenAirCmd& cmd) {
    if (s_cmdMux) {
        xSemaphoreTake(s_cmdMux, portMAX_DELAY);
        s_pendingCmd = cmd;
        xSemaphoreGive(s_cmdMux);
    }
}

// ── NVS config ───────────────────────────────────────────────────────────────
bool openairLoadConfig(std::string& addr) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char buf[24] = {};
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, NVS_ADDR, buf, &len);
    nvs_close(h);
    if (err != ESP_OK || buf[0] == '\0') return false;
    addr = buf;
    return true;
}

void openairSaveConfig(const std::string& addr) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_ADDR, addr.c_str());
    nvs_commit(h);
    nvs_close(h);
}

static void load_config_internal() {
    std::string raw;
    if (!openairLoadConfig(raw) || raw.empty()) {
        s_configured = false;
        return;
    }
    // Normalise: uppercase, strip colons
    std::string norm;
    for (char c : raw) if (c != ':') norm += (char)toupper((unsigned char)c);
    if (norm.size() != 12) { s_configured = false; return; }

    s_targetAddr = norm;
    s_configured = true;
    if (!s_dataMux)   s_dataMux   = xSemaphoreCreateMutex();
    if (!s_cmdMux)    s_cmdMux    = xSemaphoreCreateMutex();
    if (!s_notifySem) s_notifySem = xSemaphoreCreateBinary();
}

void openairBleInit() {
    load_config_internal();
    if (s_configured)
        ESP_LOGI(TAG, "cfg loaded target=%s", s_targetAddr.c_str());
    else
        ESP_LOGI(TAG, "not configured");
}

void openairBleReloadConfig() {
    load_config_internal();
    if (s_configured)
        ESP_LOGI(TAG, "cfg reloaded target=%s", s_targetAddr.c_str());
    else
        ESP_LOGI(TAG, "not configured (cleared)");
}

#else // !ENABLE_BLE ─────────────────────────────────────────────────────────

void openairBleInit()         {}
void openairBleReloadConfig() {}
void openairBleHandleAd(const NimBLEAdvertisedDevice*) {}
uint32_t openairLastSeenMs()  { return 0; }
bool openairIsConfigured()    { return false; }
OpenAirData openairGetData()  { return {}; }
void openairSetCmd(const OpenAirCmd&) {}
bool openairPollOnce()        { return false; }
bool openairLoadConfig(std::string&)         { return false; }
void openairSaveConfig(const std::string&)   {}

#endif // ENABLE_BLE
