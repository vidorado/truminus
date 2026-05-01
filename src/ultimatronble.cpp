#ifdef CYD
#include "ultimatronble.hpp"
#include "victronble.hpp"
#include <Preferences.h>
#include <NimBLEDevice.h>

// ── NVS ───────────────────────────────────────────────────────────────────
static const char* NVS_NS   = "batt";
static const char* NVS_ADDR = "addr";   // 12 uppercase hex chars, no separators

bool ultimatronLoadConfig(String& addr) {
    Preferences p;
    p.begin(NVS_NS, true);
    if (!p.isKey(NVS_ADDR)) { p.end(); return false; }
    addr = p.getString(NVS_ADDR, "");
    p.end();
    return addr.length() == 12;
}

void ultimatronSaveConfig(const String& addr) {
    Preferences p;
    p.begin(NVS_NS, false);
    p.putString(NVS_ADDR, addr);
    p.end();
}

// ── Module state ──────────────────────────────────────────────────────────
static bool          s_configured     = false;
static String        s_targetAddr;        // 12 uppercase hex (no colons)

static SemaphoreHandle_t s_dataMux    = nullptr;
static SemaphoreHandle_t s_rxSem      = nullptr;
static UltratronData     s_data       = {};

static TaskHandle_t  s_taskHandle     = nullptr;

// ── BLE response buffer ───────────────────────────────────────────────────
static uint8_t  s_rxBuf[64] = {};
static int      s_rxLen     = 0;

// ── Notification callback (runs on BLE stack task) ────────────────────────
static void notifyCb(NimBLERemoteCharacteristic* /*ch*/,
                     uint8_t* data, size_t len, bool /*isNotify*/) {
    // Log raw bytes so we can verify the response format
    Serial.printf("[ult] notify %d bytes:", (int)len);
    for (size_t i = 0; i < len && i < 8; i++) Serial.printf(" %02X", data[i]);
    if (len > 8) Serial.print(" ...");
    Serial.println();

    int space = (int)sizeof(s_rxBuf) - s_rxLen;
    int copy  = min((int)len, space);
    if (copy > 0) {
        memcpy(s_rxBuf + s_rxLen, data, copy);
        s_rxLen += copy;
    }
    // Complete response: dd 03 00 <data_len> [data] chkH chkL 77
    if (s_rxLen >= 4 && s_rxBuf[0] == 0xdd && s_rxBuf[1] == 0x03 && s_rxBuf[2] == 0x00) {
        int expected = 4 + (int)s_rxBuf[3] + 3;
        Serial.printf("[ult] notify: rxLen=%d expected=%d\n", s_rxLen, expected);
        if (s_rxLen >= expected && s_rxSem)
            xSemaphoreGive(s_rxSem);
    } else if (s_rxLen >= 4) {
        Serial.printf("[ult] notify: header mismatch %02X %02X %02X\n",
                      s_rxBuf[0], s_rxBuf[1], s_rxBuf[2]);
    }
}

// ── Format "AABBCCDDEEFF" → "AA:BB:CC:DD:EE:FF" ──────────────────────────
static std::string formatMac(const String& compact) {
    if (compact.length() != 12) return compact.c_str();
    std::string s;
    for (int i = 0; i < 12; i += 2) {
        if (i > 0) s += ':';
        s += compact[i];
        s += compact[i + 1];
    }
    return s;
}

// ── Parse response packet ─────────────────────────────────────────────────
// Offsets in the full response (including 4-byte header dd 03 00 len):
//   [4-5]  pack voltage  uint16BE  /100 → V
//   [6-7]  current       int16BE   /100 → A (neg = charging)
//   [23]   SOC           uint8     %
//   [27-28] NTC temp 1   int16BE   (val - 2731) / 10 → °C
static void parseResponse(const uint8_t* d, int len) {
    if (len < 27 || d[0] != 0xdd || d[1] != 0x03 || d[2] != 0x00) return;

    uint16_t rawV = ((uint16_t)d[4] << 8) | d[5];
    int16_t  rawA = (int16_t)(((uint16_t)d[6] << 8) | d[7]);

    UltratronData ud = {};
    ud.battV  = rawV / 100.0f;
    ud.battA  = rawA / 100.0f;
    ud.soc    = d[23];
    ud.valid  = true;
    ud.lastMs = millis();

    if (len >= 30) {
        // d[26] = number of temperature sensors; first sensor at d[27..28]
        int16_t rawT = (int16_t)(((uint16_t)d[27] << 8) | d[28]);
        ud.tempC = (rawT - 2731) / 10.0f;
    }

    Serial.printf("[ult] SOC=%u%% V=%.1fV A=%.2fA T=%.1f°C\n",
                  ud.soc, ud.battV, ud.battA, ud.tempC);

    xSemaphoreTake(s_dataMux, portMAX_DELAY);
    s_data = ud;
    xSemaphoreGive(s_dataMux);
}

// ── Single poll: connect → subscribe → query → parse → disconnect ─────────
static bool pollUltratron() {
    uint32_t t0 = millis();
    Serial.printf("[ult] poll start → %s\n", s_targetAddr.c_str());

    victronBleSuspend();
    vTaskDelay(pdMS_TO_TICKS(400));

    std::string macStr = formatMac(s_targetAddr);
    NimBLEAddress addr(macStr);

    NimBLEClient* client = NimBLEDevice::createClient();
    client->setConnectTimeout(5);

    uint32_t tConn = millis();
    if (!client->connect(addr)) {
        Serial.printf("[ult] connect failed (+%lums)\n", millis() - t0);
        NimBLEDevice::deleteClient(client);
        victronBleResume();
        return false;
    }
    Serial.printf("[ult] connected (+%lums, conn took %lums)\n",
                  millis() - t0, millis() - tConn);

    uint32_t tSvc = millis();
    NimBLERemoteService* svc = client->getService("ff00");
    if (!svc) {
        Serial.printf("[ult] service ff00 not found (+%lums)\n", millis() - t0);
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        victronBleResume();
        return false;
    }

    NimBLERemoteCharacteristic* notifChar = svc->getCharacteristic("ff01");
    NimBLERemoteCharacteristic* writeChar = svc->getCharacteristic("ff02");
    if (!notifChar || !writeChar) {
        Serial.printf("[ult] ff01/ff02 chars not found (+%lums)\n", millis() - t0);
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        victronBleResume();
        return false;
    }
    Serial.printf("[ult] service+chars discovered (+%lums, disc took %lums)\n",
                  millis() - t0, millis() - tSvc);

    uint32_t tSub = millis();
    if (!notifChar->subscribe(true, notifyCb)) {
        Serial.printf("[ult] subscribe failed (+%lums)\n", millis() - t0);
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        victronBleResume();
        return false;
    }
    Serial.printf("[ult] subscribed (+%lums, sub took %lums)\n",
                  millis() - t0, millis() - tSub);

    // BMS often ignores the first command — retry up to 5 times (same as reference impl)
    const uint8_t cmd[] = {0xDD, 0xA5, 0x03, 0x00, 0xFF, 0xFD, 0x77};
    bool got = false;
    for (int attempt = 1; attempt <= 5 && !got; attempt++) {
        // Reset buffer and semaphore before each attempt
        s_rxLen = 0;
        memset(s_rxBuf, 0, sizeof(s_rxBuf));
        while (xSemaphoreTake(s_rxSem, 0) == pdTRUE) {}

        // false = Write Without Response — JBD BMS doesn't ack writes
        writeChar->writeValue(cmd, sizeof(cmd), false);
        uint32_t tWait = millis();
        Serial.printf("[ult] command sent (attempt %d), waiting...\n", attempt);

        got = (xSemaphoreTake(s_rxSem, pdMS_TO_TICKS(2000)) == pdTRUE);
        Serial.printf("[ult] attempt %d: %s (%lums)\n",
                      attempt, got ? "OK" : "timeout", millis() - tWait);
    }

    client->disconnect();
    NimBLEDevice::deleteClient(client);
    victronBleResume();

    if (!got) {
        Serial.printf("[ult] poll failed (total %lums)\n", millis() - t0);
        return false;
    }

    parseResponse(s_rxBuf, s_rxLen);
    Serial.printf("[ult] poll done (total %lums)\n", millis() - t0);
    return true;
}

// ── Periodic poll task ────────────────────────────────────────────────────
static void ultimatronTask(void* /*arg*/) {
    vTaskDelay(pdMS_TO_TICKS(22000));   // let Victron and WiFi/MQTT settle first
    Serial.println("[ult] poll task started");

    for (;;) {
        pollUltratron();
        vTaskDelay(pdMS_TO_TICKS(30000));   // poll every 30 s
    }
}

// ── Public API ────────────────────────────────────────────────────────────
bool ultimatronIsConfigured() { return s_configured; }

UltratronData ultimatronGetData() {
    UltratronData copy = {};
    if (s_dataMux) {
        xSemaphoreTake(s_dataMux, portMAX_DELAY);
        copy = s_data;
        xSemaphoreGive(s_dataMux);
    }
    return copy;
}

void ultimatronBleSuspend() {
    if (s_taskHandle) vTaskSuspend(s_taskHandle);
}

void ultimatronBleResume() {
    if (s_taskHandle) vTaskResume(s_taskHandle);
}

void ultimatronBleInit() {
    String addr;
    if (!ultimatronLoadConfig(addr)) return;

    s_targetAddr  = addr;
    s_configured  = true;
    s_dataMux     = xSemaphoreCreateMutex();
    s_rxSem       = xSemaphoreCreateBinary();

    // NimBLE already initialised by victronBleInit(); calling init("") is idempotent.
    xTaskCreate(ultimatronTask, "ult_batt", 6144, nullptr, 1, &s_taskHandle);
    Serial.printf("[ult] Ultimatron init ok, target=%s\n", s_targetAddr.c_str());
}

#endif // CYD
