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
//   4-5   BatteryType         chemistry 0=Pb (lead-acid), 1=lithium — config field
//   6-7   Power               max power 0=2.0 kW, 1=1.2 kW (inverted) — config field
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
#include "i18n.hpp"
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
// True once a telemetry frame has been read for the current target — i.e. the
// handshake succeeded and the unit accepts us as a paired client. Reset when the
// config (re)loads; the pairing assistant watches it to auto-close on success.
static volatile bool     s_paired      = false;

// Lost-pairing detection. A unit that dropped our pairing still accepts the BLE
// connection but withholds the handshake reply (it shows "Bt" and waits for a
// long-press), so we connect but never read telemetry. Count consecutive polls
// that reach the unit without yielding a frame; after OA_UNPAIR_FAILS, raise
// s_acNeedsPair so the UI prompts a re-pair instead of silently swallowing
// commands. A telemetry read clears both. Kept separate from s_paired so the
// supervisor's pairing-priority (which polls harder) is NOT re-triggered in the
// field — we only want to inform, not hammer the shared radio.
static volatile bool     s_acNeedsPair = false;
static int               s_failStreak  = 0;
static const int         OA_UNPAIR_FAILS = 3;

static SemaphoreHandle_t s_dataMux     = nullptr;
static SemaphoreHandle_t s_cmdMux      = nullptr;
static OpenAirData       s_data        = {};
static OpenAirCmd        s_pendingCmd  = {};
// Only write a setpoint frame when the user actually changed something. Writing
// every poll makes the unit re-apply the command each cycle (beep + LED flash).
static bool              s_hasPendingCmd = false;
// Snapshot of the unit's own write-region (the first 30 bytes of the telemetry
// frame — every writable field: BatteryType, Power, TempScale, LED, flaps, …).
// Commands are built ON TOP of this so we only ever change the four fields the
// user controls and never disturb the unit's configuration (e.g. a lithium
// BatteryType, the LED, or the reserved slot). Invalid until the first read.
static uint8_t           s_writeShadow[30] = {};
static bool              s_haveShadow      = false;

static volatile uint32_t s_lastSeenMs  = 0;

// Full advertised address (incl. type) of the matched peer, captured at scan
// time.  We connect with THIS rather than NimBLEAddress(mac, 0): the OpenAir
// unit may advertise a random (type 1) address, and forcing public (type 0)
// makes the link-layer connect time out.
static NimBLEAddress s_advAddr;
static bool          s_haveAdvAddr     = false;

// Device id written in the handshake. A real-unit btsnoop of the app pairing
// shows writeId is exactly these 8 bytes (no service-data prefix), and the same
// value appears across separate captures — it is stable, not per-phone. The
// unit stores it against the paired client; sending the app's id lets us attach
// as an already-known client, so the unit accepts us without re-entering "Bt".
static const uint8_t TRUMINUS_ID8[8]  = { 0x01, 0xe8, 0x77, 0x07, 0xed, 0xe0, 0xc8, 0x25 };

// Cyclic-poll cadence: the driver connects, reads telemetry, sends any pending
// command, and disconnects each cycle. The interval is bounded by shared-radio
// contention — each poll is a GATT connect on the C6 that pauses the Victron/BLE
// scan for its duration — NOT by the unit's "Bt" indicator (that is the pairing
// long-press state; a known-client reconnect does not re-enter it, so cyclic
// polling does not blink it). Poll immediately when the user changed something
// (a pending command needs low latency).
static const uint32_t POLL_INTERVAL_MS = 4000;
static uint32_t          s_lastPollMs   = 0;

// Persistent GATT client: kept alive across polls (the LINK is still cyclic —
// we disconnect after each poll) so the discovered attribute table survives.
// Reconnecting with deleteAttributes=false lets getService() return the cached
// service with zero ATT traffic, skipping the full-database discovery whose
// SDIO burst spikes internal DRAM (and tipped the post-OTA self-test's heap
// floor). Full discovery runs only on the first poll or after a cache drop.
// The object holds no radio resource while disconnected; it lives in PSRAM
// (NimBLE host heap is external), so keeping it costs no internal DRAM.
static NimBLEClient*     s_client     = nullptr;
static bool             s_gattCached = false;   // s_client holds a valid discovered DB

// Warm-hold: after a command write, keep the GATT link open this long and send
// any follow-up command instantly (each tap re-arms the window), so rapid
// adjustments — e.g. nudging a flap to a position — don't pay a fresh connect
// each time. The hold runs INSIDE openairPollOnce on the single supervisor task,
// so the Victron scan is paused for its duration; that is what makes it safe
// (the abandoned persistent link crashed the shared C6 radio by holding the
// connection open *while scanning*). Other BLE sensors just pause briefly.
static const uint32_t OA_CMD_HOLD_MS = 6000;
// A swing command starts the louver oscillating; the user then waits for it to
// reach the wanted position and taps "fix" to stop it. Hold the link open longer
// after a swing so that follow-up "fix" tap still lands on the warm connection.
static const uint32_t OA_CMD_HOLD_SWING_MS = 15000;

// Hold window for the command just written: longer right after a swing so the
// user can catch the flap and stop it with an instant "fix".
static uint32_t cmdHoldMs(const OpenAirCmd& c) {
    bool swing = ((c.configDirty & OA_CFG_FLAP1) && c.flaps1 == OA_FLAP_SWING) ||
                 ((c.configDirty & OA_CFG_FLAP2) && c.flaps2 == OA_FLAP_SWING);
    return swing ? OA_CMD_HOLD_SWING_MS : OA_CMD_HOLD_MS;
}

// Telemetry older than this (3 missed 15 s polls) means the A/C has dropped off
// — the UI treats it as disconnected even though the last frame is still cached.
static const uint32_t AC_STALE_MS = 45000;

// Once the A/C is confirmed unreachable (s_acNeedsPair), stop hammering it: a
// stuck cool command would otherwise force a connect attempt EVERY supervisor
// cycle, each one blocking the shared radio for seconds and starving the Victron
// (SmartSolar) / Ultimatron (battery) reads. Back off to this interval and
// fast-fail the connect so the periodic retry is cheap; the command is parked
// and applied once the unit is reachable again.
static const uint32_t OA_BACKOFF_MS = 60000;

// Notification buffer + semaphore: the host callback copies the frame and signals;
// openairPollOnce() waits on it. Keep the callback tiny (no float-printf/mutex —
// they overflow the host task's small stack).
static uint8_t           s_notifyBuf[128] = {};
static int               s_notifyLen      = 0;
static SemaphoreHandle_t s_notifySem      = nullptr;

// ── Notification callback ────────────────────────────────────────────────────
// Telemetry arrives once subscribed. Copy it and signal the waiting poll.
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

    // Snapshot the unit's write-region so outgoing commands preserve every field
    // we don't explicitly set (BatteryType, Power, TempScale, LED, flaps, …).
    memcpy(s_writeShadow, bytes, sizeof(s_writeShadow));
    s_haveShadow = true;
    s_paired     = true;   // a reply means the handshake was accepted
    s_acNeedsPair = false; // the unit is talking to us again
    s_failStreak  = 0;

    OpenAirData d = {};
    d.batteryType        = (int)readU16BE(bytes, 4);   // field 2  (config)
    d.power              = (int)readU16BE(bytes, 6);   // field 3  (config)
    d.uPowerState        = (int)readU16BE(bytes, 10);  // control half — for UI reconcile
    d.uMode              = (int)readU16BE(bytes, 12);
    d.uTempTenths        = (int)readU16BE(bytes, 14);
    d.uBlower            = (int)readU16BE(bytes, 16);
    d.uFlaps1            = (int)readU16BE(bytes, 26);  // field 11 — for UI seed
    d.uFlaps2            = (int)readU16BE(bytes, 28);  // field 12
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

    ESP_LOGI(TAG, "telemetry probe1=%.0f probe2=%.0f blower=%d%% comp_rpm=%d err=%d batt=%d pwr=%d",
             d.probe1C, d.probe2C, d.blowerSpeedPct, d.compressorSpeedRpm, d.errors,
             d.batteryType, d.power);

    xSemaphoreTake(s_dataMux, portMAX_DELAY);
    s_data = d;
    xSemaphoreGive(s_dataMux);
    return true;
}

static void writeU16BE(uint8_t* b, int off, unsigned v) {
    b[off]     = (uint8_t)(v >> 8);
    b[off + 1] = (uint8_t)(v & 0xFF);
}

// ── Command frame builder ─────────────────────────────────────────────────────
// Builds the 30-byte command by starting from the unit's own last write-region
// (s_writeShadow) and overwriting ONLY the fields we intend to change — the four
// live user controls (PowerState/Mode/Temp/BlowerSpeed) plus the freshly-stamped
// clock, and the two flaps ONLY when their dirty bit is set (the user changed a
// louver). Everything else (BatteryType, Power, TempScale, LedBright/Color,
// ScheduledTime, and untouched flaps) is echoed back exactly as the unit reported
// it, so we never reconfigure the appliance. Returns false
// if no telemetry has been read yet (never send a blind default frame). Output is
// 60 ASCII lowercase hex chars (the wire value is those code units).
static bool buildWriteFrame(const OpenAirCmd& cmd, char out[61]) {
    if (!s_haveShadow) return false;

    uint8_t f[30];
    memcpy(f, s_writeShadow, sizeof(f));

    uint32_t rtc = (uint32_t)((esp_timer_get_time() / 1000000ULL) % 86400UL);
    f[0] = (uint8_t)(rtc >> 24); f[1] = (uint8_t)(rtc >> 16);
    f[2] = (uint8_t)(rtc >> 8);  f[3] = (uint8_t)(rtc & 0xFF);   // [0-3] RealTimeClock

    writeU16BE(f, 10, (unsigned)cmd.powerState);   // [10-11] PowerState
    writeU16BE(f, 12, (unsigned)cmd.mode);         // [12-13] Mode
    writeU16BE(f, 14, (unsigned)cmd.tempTenths);   // [14-15] Temp (tenths °C)
    writeU16BE(f, 16, (unsigned)cmd.blowerSpeed);  // [16-17] BlowerSpeed

    // Config fields: normally echoed from the shadow (memcpy above). Overwrite
    // them ONLY when the user changed them on the Peripherals screen (dirty bit),
    // which the UI allows only while the unit is OFF.
    if (cmd.configDirty & OA_CFG_BATTERY) writeU16BE(f, 4, (unsigned)cmd.batteryType);
    if (cmd.configDirty & OA_CFG_POWER)   writeU16BE(f, 6, (unsigned)cmd.power);
    // Flaps: echoed from the shadow unless the user changed that louver.
    if (cmd.configDirty & OA_CFG_FLAP1)   writeU16BE(f, 26, (unsigned)cmd.flaps1);  // [26-27] Flaps1Mode
    if (cmd.configDirty & OA_CFG_FLAP2)   writeU16BE(f, 28, (unsigned)cmd.flaps2);  // [28-29] Flaps2Mode

    for (int i = 0; i < 30; i++) snprintf(out + i * 2, 3, "%02x", f[i]);
    return true;
}

// Wait (bounded) for an async disconnect to finish. deleteClient() will not
// free a still-connected client, and a write to a half-open link hangs.
static void waitDisconnected(NimBLEClient* client) {
    if (!client) return;
    client->disconnect();
    for (int i = 0; i < 20 && client->isConnected(); i++) vTaskDelay(pdMS_TO_TICKS(50));
}

// End a poll but KEEP the client object + its cached GATT DB for the next poll
// (the fast path). Only the link is torn down.
static void parkClient() {
    waitDisconnected(s_client);
}

// Fully tear down: disconnect, free the client and invalidate the cache. Used on
// a GATT-level failure (stale/missing attributes) and on a config/target change,
// so the next poll rebuilds a clean client and rediscovers.
static void dropClient() {
    waitDisconnected(s_client);
    if (s_client) { NimBLEDevice::deleteClient(s_client); s_client = nullptr; }
    s_gattCached = false;
}

// ── Public API ───────────────────────────────────────────────────────────────

// One cyclic poll: connect → discover → Service Changed → handshake → subscribe
// → read one telemetry frame → (optionally) write a pending command → read back →
// disconnect. Runs on the supervisor task. A short-lived connection keeps the
// NimBLE/SDIO footprint small (a persistent link fought the Victron scan and
// destabilised the C6 link); the cost is the unit briefly showing "Bt" per poll,
// which the cadence gate below keeps infrequent.
bool openairPollOnce() {
    if (!s_configured) return false;

    // Cadence gate: normally poll every POLL_INTERVAL_MS, or immediately when a
    // user command is pending (low-latency button response). But once the unit
    // is confirmed unreachable, back off to OA_BACKOFF_MS even with a command
    // pending — otherwise a cool command we can't deliver hammers the radio every
    // cycle and starves the other BLE reads. The command stays parked for when
    // the unit returns.
    bool cmdPending;
    xSemaphoreTake(s_cmdMux, portMAX_DELAY);
    cmdPending = s_hasPendingCmd;
    xSemaphoreGive(s_cmdMux);
    uint32_t interval = s_acNeedsPair ? OA_BACKOFF_MS : POLL_INTERVAL_MS;
    bool urgent = cmdPending && !s_acNeedsPair;
    if (!urgent && s_lastPollMs && (millis() - s_lastPollMs) < interval)
        return false;
    s_lastPollMs = millis();

    uint32_t t0 = millis();
    NimBLEAddress addr = s_haveAdvAddr ? s_advAddr
                                       : NimBLEAddress(formatMac(s_targetAddr), 0);

    // Reuse the persistent client so its discovered GATT DB survives across polls
    // (see s_client). Create it once; a cold poll (no cache) does full discovery,
    // a warm one reconnects with deleteAttributes=false and skips it.
    if (!s_client) { s_client = NimBLEDevice::createClient(); s_gattCached = false; }
    NimBLEClient* client = s_client;
    if (!client) {
        // Pool exhausted — every slot still holds an unfreed client. Never
        // dereference null (setConnectTimeout would panic). A freed slot returns.
        ESP_LOGW(TAG, "createClient failed (client pool exhausted)");
        return false;
    }
    // Fast-fail while backed off: one short attempt so a periodic retry against
    // an absent unit doesn't freeze the supervisor (and the Victron/Ultimatron
    // reads behind it) for the full 3×5 s connect budget.
    client->setConnectTimeout(s_acNeedsPair ? 3000 : 5000);

    // NOTE: do NOT force a fast connection interval here. A 7.5–15 ms interval
    // speeds the GATT setup but hammers the radio the ESP32-C6 shares with WiFi,
    // starving the WSS cloud tunnel (the cloud icon blinks / drops). Keep the
    // stack default; command latency is reduced instead by scheduling (A/C polled
    // first, scan aborted on demand) which does not increase radio airtime.

    // Telemetry notifications are 124 bytes and cannot fit the 23-byte default
    // MTU (max notify = MTU-3). The app negotiates a large MTU right after connect.
    NimBLEDevice::setMTU(247);

    bool warm = s_gattCached;   // reconnect keeping the cached attribute table?

    ESP_LOGI(TAG, "connecting %s (type=%d)%s",
             addr.toString().c_str(), (int)addr.getType(), warm ? " [cached]" : "");
    bool connected = false;
    int  maxAttempts = s_acNeedsPair ? 1 : 3;   // single attempt while backed off
    for (int attempt = 1; attempt <= maxAttempts && !connected; attempt++) {
        connected = client->connect(addr, /*deleteAttributes=*/ !warm);
        if (!connected && attempt < maxAttempts) vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!connected) {
        ESP_LOGW(TAG, "connect failed (+%ums)", (unsigned)(millis() - t0));
        // A reachability miss doesn't invalidate the cached DB — keep the client
        // (and the cache) parked for when the unit returns.
        parkClient(); return false;
    }

    // We reached the unit. Tentatively count this poll as a failure; a telemetry
    // read (parseNotification) clears the streak. If we connect this many times
    // without ever reading a frame, the unit has dropped our pairing (it accepts
    // the link but withholds the handshake, showing "Bt") — flag it so the UI can
    // prompt a re-pair. Only counts post-connect, so an out-of-range unit (connect
    // fails above) never trips it.
    if (++s_failStreak >= OA_UNPAIR_FAILS) s_acNeedsPair = true;

    // Cold poll only: full GATT discovery (a per-UUID filtered discovery sometimes
    // fails on 128-bit custom UUIDs even when the service is present). A warm poll
    // skips this entirely — getService() below returns the cached service.
    if (!warm && !client->discoverAttributes()) {
        ESP_LOGW(TAG, "attribute discovery failed");
        dropClient(); return false;
    }
    NimBLERemoteService* svc = client->getService(SVC_UUID);
    if (!svc) { ESP_LOGW(TAG, "target service not found"); dropClient(); return false; }
    NimBLERemoteCharacteristic* hsChar   = svc->getCharacteristic(HANDSHAKE_UUID);
    NimBLERemoteCharacteristic* dataChar = svc->getCharacteristic(DATA_UUID);
    if (!hsChar || !dataChar) {
        ESP_LOGW(TAG, "characteristics not found");
        dropClient(); return false;
    }
    // Attributes are valid → future reconnects can go warm.
    s_gattCached = true;

    // Enable Service Changed indications (0x2A05 in GATT service 0x1801) BEFORE
    // the handshake — the app's btsnoop shows this exact order. Best-effort.
    if (NimBLERemoteService* gatt = client->getService("1801")) {
        if (NimBLERemoteCharacteristic* sc = gatt->getCharacteristic("2A05"))
            sc->subscribe(false, nullptr, true);   // false = indications (0x0002)
    }

    // Handshake = the app's writeId: exactly 8 raw id bytes, Write *Request*
    // (response=true, the char is Write-only). On first pairing the unit does not
    // answer until the user long-presses ON/OFF ("Bt"); NimBLE's ATT timeout
    // covers the wait. Once paired, the WriteRsp is immediate.
    if (!hsChar->writeValue(TRUMINUS_ID8, sizeof(TRUMINUS_ID8), true)) {
        ESP_LOGW(TAG, "handshake write failed");
        // A write failure on a warm reconnect may mean a stale cached handle —
        // drop so the next poll rediscovers.
        dropClient(); return false;
    }

    // Subscribe; drain any stale signal before we start waiting on notifications.
    while (xSemaphoreTake(s_notifySem, 0) == pdTRUE) {}
    if (!dataChar->subscribe(true, notifyCb)) {
        ESP_LOGW(TAG, "subscribe failed");
        dropClient(); return false;
    }

    // Grab any pending command up front so we can write it as early as possible.
    OpenAirCmd cmd = {};
    bool hasCmd = false;
    xSemaphoreTake(s_cmdMux, portMAX_DELAY);
    cmd = s_pendingCmd;
    hasCmd = s_hasPendingCmd;
    s_hasPendingCmd = false;
    xSemaphoreGive(s_cmdMux);

    // Fast path: with a command AND a retained shadow (from any prior poll), write
    // immediately — no need to wait for a fresh telemetry read first, since the
    // shadow already carries every field we preserve. This removes the read-wait
    // from the command latency; the read below then captures the post-write state.
    bool wrote = false;
    if (hasCmd && s_haveShadow) {
        char frame[61] = {};
        if (buildWriteFrame(cmd, frame)) {
            dataChar->writeValue((const uint8_t*)frame, 60, false);
            ESP_LOGI(TAG, "cmd: ps=%d mode=%d temp=%.1f°C fan=%d",
                     cmd.powerState, cmd.mode, cmd.tempTenths / 10.0f, cmd.blowerSpeed);
            wrote  = true;
            hasCmd = false;
        }
    }

    // Read one telemetry frame: the post-write echo when we just wrote (short
    // wait), otherwise the current state — a longer wait only when we still need
    // a first read to seed the shadow for future fast-path commands.
    bool got = (xSemaphoreTake(s_notifySem, pdMS_TO_TICKS(wrote ? 3000 : 5000)) == pdTRUE);
    if (got) parseNotification(s_notifyBuf, s_notifyLen);
    else if (!wrote) ESP_LOGW(TAG, "no telemetry within 5 s");

    // Slow path: a command was pending but there was no shadow yet (first pairing).
    // Now that we've read one frame we can build on it; write and read the echo.
    if (hasCmd) {
        char frame[61] = {};
        if (buildWriteFrame(cmd, frame)) {
            dataChar->writeValue((const uint8_t*)frame, 60, false);
            ESP_LOGI(TAG, "cmd: ps=%d mode=%d temp=%.1f°C fan=%d",
                     cmd.powerState, cmd.mode, cmd.tempTenths / 10.0f, cmd.blowerSpeed);
            if (xSemaphoreTake(s_notifySem, pdMS_TO_TICKS(3000)) == pdTRUE)
                parseNotification(s_notifyBuf, s_notifyLen);
        } else {
            // Still no telemetry ever — keep the command pending, don't blind-write.
            xSemaphoreTake(s_cmdMux, portMAX_DELAY);
            s_pendingCmd = cmd; s_hasPendingCmd = true;
            xSemaphoreGive(s_cmdMux);
        }
    }

    // Warm-hold: once we've written at least one command this session and the
    // shadow is seeded, keep the link open briefly and flush follow-up commands
    // instantly (each one re-arms the window). Only entered after a real command
    // (not a plain telemetry poll) so idle polls still disconnect immediately.
    if ((wrote || hasCmd) && s_haveShadow) {
        uint32_t holdUntil = millis() + cmdHoldMs(cmd);
        while ((int32_t)(holdUntil - millis()) > 0 && client->isConnected()) {
            OpenAirCmd next = {};
            bool have = false;
            xSemaphoreTake(s_cmdMux, portMAX_DELAY);
            if (s_hasPendingCmd) { next = s_pendingCmd; s_hasPendingCmd = false; have = true; }
            xSemaphoreGive(s_cmdMux);
            if (!have) { vTaskDelay(pdMS_TO_TICKS(40)); continue; }

            char frame[61] = {};
            if (!buildWriteFrame(next, frame)) continue;
            dataChar->writeValue((const uint8_t*)frame, 60, false);
            ESP_LOGI(TAG, "cmd (hold): ps=%d mode=%d temp=%.1f°C fan=%d flaps=%d/%d",
                     next.powerState, next.mode, next.tempTenths / 10.0f,
                     next.blowerSpeed, next.flaps1, next.flaps2);
            // Capture the post-write echo so the shadow tracks the unit, then
            // re-arm the window for the next tap.
            if (xSemaphoreTake(s_notifySem, pdMS_TO_TICKS(1500)) == pdTRUE)
                parseNotification(s_notifyBuf, s_notifyLen);
            holdUntil = millis() + cmdHoldMs(next);
        }
    }

    // Keep the client + its cached GATT DB for the next (warm) poll; only the
    // link is torn down.
    parkClient();
    ESP_LOGI(TAG, "poll %s (+%ums)%s", got ? "ok" : "no-telemetry",
             (unsigned)(millis() - t0), warm ? " [cached]" : "");
    return got;
}

void openairBleHandleAd(const NimBLEAdvertisedDevice* dev) {
    if (!s_configured || !dev) return;

    // Match by configured MAC (normalised, no colons, uppercase).
    std::string devAddr;
    for (char c : dev->getAddress().toString())
        if (c != ':') devAddr += (char)toupper((unsigned char)c);
    if (devAddr != s_targetAddr) return;

    bool first = (s_lastSeenMs == 0);
    s_lastSeenMs = millis();
    s_advAddr    = dev->getAddress();   // keep the real type for the connect
    s_haveAdvAddr = true;
    if (first) ESP_LOGW(TAG, "advertising seen: %s", dev->getAddress().toString().c_str());
}

bool openairCmdPending() {
    if (!s_cmdMux) return false;
    xSemaphoreTake(s_cmdMux, portMAX_DELAY);
    bool p = s_hasPendingCmd;
    xSemaphoreGive(s_cmdMux);
    return p;
}

uint32_t openairLastSeenMs() { return s_lastSeenMs; }
uint32_t openairLastPollMs() { return s_lastPollMs; }

bool openairIsConfigured() { return s_configured; }

bool openairPaired() { return s_paired; }

bool openairNeedsPair() { return s_acNeedsPair; }

bool openairConnected() {
    if (!s_dataMux) return false;
    xSemaphoreTake(s_dataMux, portMAX_DELAY);
    bool live = s_data.valid && (millis() - s_data.lastMs) < AC_STALE_MS;
    xSemaphoreGive(s_dataMux);
    return live;
}

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
        s_hasPendingCmd = true;   // send it on the next poll, then stop re-sending
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

// Localised error titles — decoded from the app's error.dart::listaErrores
// (raw Errors value, decimal).  0 = treated as "no fault" (see skill note);
// unknown codes return nullptr so the caller shows nothing.  t() returns the
// current-language string, so the status bar follows the UI language.
const char* openairErrorTitle(int errors) {
    switch (errors) {
        case 1:  return t(TK::AC_ERR_EFAN);
        case 2:  return t(TK::AC_ERR_BLOWER);
        case 6:  return t(TK::AC_ERR_FREEZE);
        case 9:  return t(TK::AC_ERR_TILT);
        case 18: return t(TK::AC_ERR_FLAP1);
        case 19: return t(TK::AC_ERR_FLAP2);
        default: return nullptr;
    }
}

// Localised long description for the error modal body.  nullptr for no fault.
const char* openairErrorDesc(int errors) {
    switch (errors) {
        case 1: case 2: case 6: return t(TK::AC_DESC_STOP);   // critical, unit stops
        case 9:                 return t(TK::AC_DESC_TILT);   // temporary
        case 18: case 19:       return t(TK::AC_DESC_FLAP);   // limited operation
        default:                return nullptr;
    }
}

static void load_config_internal() {
    s_paired = false;   // fresh config → not yet confirmed paired this session
    s_acNeedsPair = false;
    s_failStreak  = 0;
    // Invalidate the cached GATT DB: the target MAC may have changed, so the next
    // poll must rediscover (connect with deleteAttributes=true). A plain bool
    // write — the poll task owns the s_client object's lifecycle, so we never
    // free it from here (no cross-task use-after-free).
    s_gattCached = false;
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
uint32_t openairLastPollMs()  { return 0; }
bool openairIsConfigured()    { return false; }
OpenAirData openairGetData()  { return {}; }
void openairSetCmd(const OpenAirCmd&) {}
bool openairPollOnce()        { return false; }
bool openairNeedsPair()       { return false; }
bool openairConnected()       { return false; }
bool openairLoadConfig(std::string&)         { return false; }
void openairSaveConfig(const std::string&)   {}
const char* openairErrorTitle(int)           { return nullptr; }
const char* openairErrorDesc(int)            { return nullptr; }

#endif // ENABLE_BLE
