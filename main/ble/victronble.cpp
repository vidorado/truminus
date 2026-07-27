#include "victronble.hpp"
#include "victron_codec.hpp"
#include "ultimatronble.hpp"
#include "tankble.hpp"
#include "multiplusble.hpp"
#include "openairble.hpp"
#include "wifi_manager.hpp"
#include "logs.hpp"
#include "heapdiag.hpp"
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
static volatile bool s_bleTeardown      = false;  // request supervisor exit for NimBLE deinit

// DIAGNOSTIC: log each DISTINCT advertiser MAC once (up to this many) so we
// can see how many different devices the radio actually delivers — not 40
// copies of the one aggressive advertiser.
static char s_seenMacs[40][18];
static int  s_seenCount = 0;

// DIAGNOSTIC: total adverts seen in the current scan window (any device).
// Plain (not volatile): written from the NimBLE host task during the scan,
// read by the supervisor only after the window closes (s_supervisorInScan=0).
static uint32_t s_scanAdvCount = 0;

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
    if (mfr[6] != 0x01) return;             // 0x01 = SolarCharger; let other
                                            // record types (VE.Bus 0x0C, etc.)
                                            // fall through to their own parser
    if (mfr[9] != s_aesKey[0]) return;

    uint8_t out[16] = {};
    if (!aesCtrDecrypt(mfr + 10, 16, mfr[7], mfr[8], out)) return;

    // Field decode lives in the host-tested victron_codec module; returns false
    // on the 0x7FFF "not available" voltage sentinel.
    VictronData d = {};
    if (!victronParseRecord(out, d)) return;
    d.lastMs = millis();

    if (s_dataMux) xSemaphoreTake(s_dataMux, portMAX_DELAY);
    s_data = d;
    if (s_dataMux) xSemaphoreGive(s_dataMux);
}

// ── Victron scan callback (continuous monitoring) ─────────────────────────
class VictronScanCb : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        s_scanAdvCount++;   // diagnostic: count every advert this window

        // DIAGNOSTIC (bounded, deduped): log each DISTINCT advertiser once so
        // we can see how many different devices the radio delivers and whether
        // any configured Victron/Ultimatron MAC is among them.
        if (s_seenCount < (int)(sizeof(s_seenMacs) / sizeof(s_seenMacs[0]))) {
            std::string mac = dev->getAddress().toString();
            bool isNew = true;
            for (int i = 0; i < s_seenCount; i++)
                if (mac == s_seenMacs[i]) { isNew = false; break; }
            if (isNew) {
                strncpy(s_seenMacs[s_seenCount], mac.c_str(),
                        sizeof(s_seenMacs[0]) - 1);
                s_seenMacs[s_seenCount][sizeof(s_seenMacs[0]) - 1] = '\0';
                s_seenCount++;
                [[maybe_unused]] uint8_t b0 = 0, b1 = 0;
                [[maybe_unused]] unsigned mlen = 0;
                if (dev->haveManufacturerData()) {
                    const std::string& m = dev->getManufacturerData();
                    mlen = (unsigned)m.size();
                    if (m.size() >= 1) b0 = (uint8_t)m[0];
                    if (m.size() >= 2) b1 = (uint8_t)m[1];
                }
                LOG_BLE_PF("[ble] NEW dev #%d mac=%s type=%d rssi=%d mfrlen=%u mfrid=%02X%02X\n",
                           s_seenCount, mac.c_str(),
                           (int)dev->getAddress().getType(),
                           dev->getRSSI(), mlen, b1, b0);
            }
        }
        // DIAGNOSTIC: feed the Ultimatron observer so we know whether the BMS
        // is actually advertising before we sink 16 s into a blind connect.
        ultimatronBleHandleAd(dev);

        // Feed the OpenAir observer so we know when the A/C is in range.
        openairBleHandleAd(dev);

        // Diagnostic: log every advertisement that matches the target MAC so
        // we can tell "the device isn't advertising" from "it advertises but
        // without Victron manufacturer data".  Logged once per scan window
        // because NimBLE de-dupes by default.
        bool addrMatch = false;
        if (s_targetAddr.size() > 0) {
            std::string devAddr = normaliseAddr(dev->getAddress().toString());
            addrMatch = (devAddr == s_targetAddr);
        }
        if (addrMatch) {
            if (dev->haveManufacturerData()) {
                const std::string& mfr = dev->getManufacturerData();
                [[maybe_unused]] uint8_t b0 = mfr.size() >= 1 ? (uint8_t)mfr[0] : 0;
                [[maybe_unused]] uint8_t b1 = mfr.size() >= 2 ? (uint8_t)mfr[1] : 0;
                LOG_BLE_PF("[ble] adv target mfr_len=%u mfr_id=%02X%02X\n",
                           (unsigned)mfr.size(), b1, b0);
            } else {
                LOG_BLE_PL("[ble] adv target — no manufacturer data");
            }
        }

        // DIAGNOSTIC: log every Victron-branded advert (company id 0x02E1)
        // with its MAC + record type, so we can see which Victron devices are
        // actually in range and compare against the configured Solar/Multiplus
        // MACs.  record byte: 0x01=SolarCharger, 0x0C=VE.Bus(Multiplus).
        if (dev->haveManufacturerData()) {
            const std::string& m = dev->getManufacturerData();
            if (m.size() >= 7 && (uint8_t)m[0] == 0xE1 && (uint8_t)m[1] == 0x02) {
                LOG_BLE_PF("[ble] victron dev=%s rec=0x%02X len=%u\n",
                           dev->getAddress().toString().c_str(),
                           (uint8_t)m[6], (unsigned)m.size());
            }
        }

        // Route possible BTHome service-data to the tank receiver before
        // we filter on Victron's mfr-data: the tank sensor lives on a
        // different MAC and a different AD type, so the Victron path's
        // strict addr/mfr filter would otherwise drop it.
        tankBleHandleAd(dev);

        // Multiplus / VE.Bus dongle uses the same Victron envelope but a
        // different record type (0x0C), different MAC and different
        // bind key.  Hand off here too — the Solar parser below ignores
        // it because addr+mfr-id won't match.
        multiplusBleHandleAd(dev);

        if (!dev->haveManufacturerData()) return;
        if (s_targetAddr.size() > 0 && !addrMatch) return;
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
        std::string mac = normaliseAddr(dev->getAddress().toString());
        int8_t rssi = (int8_t)dev->getRSSI();
        // Deduplicate — keep the strongest RSSI and count every advert heard
        // (the count is a reception-quality metric: more adverts = better link).
        for (int i = 0; i < s_discCount; i++) {
            if (mac == s_disc[i].mac) {
                if (rssi > s_disc[i].rssi) s_disc[i].rssi = rssi;
                if (s_disc[i].seen < 0xFFFF) s_disc[i].seen++;
                return;
            }
        }
        if (s_discCount >= MAX_DISCOVERED) return;
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
        d.rssi = rssi;
        d.seen = 1;
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
    // Continuous scan (window == interval) — see bleSupervisorStart() for why the
    // C6 coexistence path makes a 50% duty cycle lose advertisements.
    scan->setInterval(160);
    scan->setWindow(160);
    scan->setMaxResults(0);
    // Count EVERY advert (no controller dedup) so BleDevice::seen reflects the
    // true reception rate — the metric used to A/B reception tweaks. Restored
    // to the default (filtered) below so the supervisor scan is unaffected.
    scan->setDuplicateFilter(0);
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

    scan->setDuplicateFilter(1);   // restore default for the supervisor scan

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
// Delay after WiFi associates before NimBLE comes up, so BLE bring-up doesn't
// overlap WiFi's transient bring-up buffers (see the heap-floor note below).
static constexpr uint32_t BLE_WIFI_SETTLE_MS = 10000;

static void bleSupervisorTask(void* /*arg*/) {
    // Sequence the shared-C6 RAM consumers. BLE/WiFi/tunnel all ride the C6 over
    // SDIO; bringing NimBLE + the Ultimatron GATT connect up *during* WiFi's
    // transient association buffers peaks internal DRAM and breached the OTA
    // self-test heap floor under weak BLE coverage (observed free internal DRAM
    // dipping to 6 KB — the failing GATT connect holds buffers longer when the
    // BMS is far). So: wait for WiFi to associate, THEN let its bring-up DRAM
    // settle before starting BLE. The GATT connect still runs well inside the
    // 60 s self-test window, so a panic/abort in that path is still caught.
    // Cost: sensors appear ~10 s later — acceptable for the heap headroom.
    for (int i = 0; i < 150; i++) {                 // ≤15 s: wait for association
        if (wifi_manager_get_status().connected) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(BLE_WIFI_SETTLE_MS));  // let WiFi bring-up DRAM settle
    LOG_BLE_PL("[ble-sup] started");

    int      failCount  = 0;
    uint32_t cycleCount = 0;
    constexpr uint32_t ULTIMATRON_EVERY_N = 6;
    // Blind-recovery pacing for a BMS that has stopped advertising (see the
    // Ultimatron poll block below). 0 = no recovery attempt yet this boot.
    uint32_t lastUltRecoverMs = 0;
    constexpr uint32_t ULT_RECOVER_MS = 60000;

    for (;;) {
        if (s_bleTeardown) break;
        if (s_bleStopped || s_bleSuspended || s_discoveryRunning) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Shorten the scan window when an A/C command is waiting: a pending
        // command otherwise sits behind a full 2 s Victron scan before the loop
        // reaches openairPollOnce(), which writes it over the persistent link.
        constexpr uint32_t SCAN_FULL_MS = 2000;
        uint32_t SCAN_MS = (openairIsConfigured() && openairCmdPending() && !openairNeedsPair())
                               ? 800 : SCAN_FULL_MS;
        if ((s_configured || tankIsConfigured() || multiplusIsConfigured() || openairIsConfigured()) && s_bleScan) {
            s_bleScan->clearResults();
            s_scanAdvCount = 0;   // diagnostic: reset per-window advert counter
            s_supervisorInScan = true;
            // NimBLE 2.x: start() is non-blocking — it returns once the GAP
            // procedure is queued, not when the duration elapses.  Wait for
            // the scan window to actually end, otherwise the Ultimatron
            // GATT connect below cuts it short and Victron ads are lost.
            bool ok = s_bleScan->start(SCAN_MS, false);
            if (ok) {
                uint32_t waited = 0;
                while (s_bleScan->isScanning() && waited < SCAN_MS + 1000) {
                    // Cut the scan short (checked every 100 ms) to reach the A/C
                    // poll sooner in the two cases that matter for responsiveness:
                    // a command is queued, or the A/C was just seen but has not
                    // been read yet (first telemetry at boot — connect on sight
                    // instead of waiting out the full window).
                    if (openairIsConfigured()) {
                        uint32_t s = openairLastSeenMs();
                        bool justSeen = s && (millis() - s) < 2000;
                        if (!openairNeedsPair() &&
                            (openairCmdPending() || (!openairPaired() && justSeen))) break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                    waited += 100;
                }
                if (s_bleScan->isScanning()) s_bleScan->stop();
                failCount = 0;
            } else {
                failCount++;
            }
            s_supervisorInScan = false;
            // DIAGNOSTIC: one line per scan window — total adverts heard and
            // how long ago (if ever) the Ultimatron MAC was seen advertising.
            uint32_t ultSeen = ultimatronLastSeenMs();
            LOG_BLE_PF("[ble-sup] scan done: adverts=%lu ult_last_seen=%s\n",
                       (unsigned long)s_scanAdvCount,
                       ultSeen ? "" : "never");
            if (ultSeen)
                LOG_BLE_PF("[ble-sup]   ult seen %lums ago\n",
                           (unsigned long)(millis() - ultSeen));
        } else {
            // Nothing to scan — still pace the loop so Ultimatron polls at the
            // expected cadence.
            vTaskDelay(pdMS_TO_TICKS(SCAN_MS));
        }

        // OpenAir PLUS A/C — HIGHEST-priority peripheral, serviced first. It holds a
        // PERSISTENT GATT link: openairPollOnce() drains streamed telemetry and
        // flushes any pending command over the open link (returning immediately so
        // this scan keeps running concurrently), and reconnects only when the link
        // is down and the unit advertised recently — the seen-recently gate and
        // reconnect pacing live inside the driver.
        // The persistent link is brought up DURING the OTA self-test as well (no
        // PendingVerify guard). With the L2-cache DRAM headroom the concurrent boot
        // peak stays far above the self-test floor, and running it inside the
        // PENDING_VERIFY window is deliberate: it makes the self-test exercise the
        // real workload, so a heap-crater OR a crash on the held-link/scan path
        // rolls back (the bootloader reverts a PENDING_VERIFY image that reboots)
        // instead of shipping a bad image. See the firmware-ota skill.
        if (openairIsConfigured()) {
            openairPollOnce();
        }

        // Ultimatron BMS — accessory, polled after the A/C. Yields to the A/C:
        // skipped while an A/C command is pending, and while the A/C is reachable
        // (seen recently) but has not completed its first read yet, so the BMS's
        // multi-second GATT connect never delays A/C telemetry or a command.
        uint32_t oaSeen  = openairLastSeenMs();
        bool oaReachable = openairIsConfigured() && oaSeen && (millis() - oaSeen) < 30000;
        // A chronically-unreachable A/C (backed off) must NOT keep priority — it
        // would starve the BMS/Victron reads. Only prioritise while it's actually
        // recoverable (not yet in the needs-pair backoff).
        bool oaPriority  = oaReachable && !openairNeedsPair()
                        && (openairCmdPending() || !openairPaired());
        // Cooldown: for a short window after ANY A/C command, keep the blocking
        // BMS poll out of the loop even once the command has flushed (pending
        // clears), so a burst of flap adjustments never lands one command behind
        // an in-flight GATT connect. Gated on reachability like oaPriority.
        constexpr uint32_t OA_CMD_COOLDOWN_MS = 15000;
        uint32_t oaCmd   = openairLastCmdMs();
        bool oaRecentCmd = oaReachable && oaCmd && (millis() - oaCmd) < OA_CMD_COOLDOWN_MS;
        // Like the A/C link, the BMS GATT poll now runs during the self-test too
        // (no PendingVerify guard): the L2 headroom keeps the concurrent peak above
        // the floor, and letting it run makes the self-test measure the real BLE
        // load rather than a lighter subset — a genuine DRAM problem then rolls back
        // instead of surfacing only after the image is already valid.
        if (ultimatronIsConfigured() && (cycleCount % ULTIMATRON_EVERY_N) == 0
            && !oaPriority && !oaRecentCmd) {
            // Preferred path: the BMS advertised in the last ~15 s, so a GATT
            // connect lands on the first attempt. The budget is kept tight
            // (2×4 s connect, 3× read) because this poll BLOCKS the shared BLE
            // loop and any long stall shows up as A/C command latency.
            uint32_t seen = ultimatronLastSeenMs();
            if (seen && (millis() - seen) < 15000) {
                ultimatronPollOnce();
            } else if (!ultimatronGetData().flowValid
                       && (lastUltRecoverMs == 0
                           || millis() - lastUltRecoverMs >= ULT_RECOVER_MS)) {
                // Trigger on flow-staleness (~35 s since the last good poll),
                // not the 5 min SOC window — so we start trying to reconnect
                // ~35 s after data stops, still paced by ULT_RECOVER_MS.
                // Recovery path: the data has gone stale and the BMS is no
                // longer advertising (many units stop advertising after their
                // first connection). Attempt one *short* blind connect (2 s, 1
                // try — not the 16 s worst case) so a single failed attempt only
                // briefly displaces the passive scans. Paced by ULT_RECOVER_MS.
                lastUltRecoverMs = millis();
                LOG_BLE_PL("[ble-sup] ult stale — short blind recovery poll");
                ultimatronPollOnce(true);
            } else {
                LOG_BLE_PL("[ble-sup] skip ult poll — not advertising recently");
            }
        }

        // A flap command queued WHILE the BMS GATT poll above was blocking this
        // single BLE loop would otherwise wait out the next scan before
        // openairPollOnce() flushes it. Flush now over the persistent link (a
        // no-op fast return when nothing is pending) so it lands immediately.
        if (openairIsConfigured() && openairCmdPending())
            openairPollOnce();

        cycleCount++;

        // Cycle pacing: 5 s scan + idle.  With aggressive=true (default) and
        // ULTIMATRON_EVERY_N=6 → Victron sample every ~5 s, Ultimatron every
        // ~30 s.  Non-aggressive falls back to 30 s idle (cold-start safety).
        uint32_t idleMs = s_aggressive ? 0 : 30000;
        if (failCount > 3) idleMs = 30000;
        if (idleMs) vTaskDelay(pdMS_TO_TICKS(idleMs));
    }
    // Teardown requested: leave the loop and exit so bleSupervisorStop() can
    // deinit the stack with no scan/poll in flight.
    s_bleTaskHandle = nullptr;
    LOG_BLE_PL("[ble-sup] exited (teardown)");
    vTaskDelete(nullptr);
}

// ── Public API ────────────────────────────────────────────────────────────
void victronBleSetAggressive(bool aggressive) { s_aggressive = aggressive; }
void victronBleStop()    { s_bleStopped    = true;  }
void victronBleStart()   { s_bleStopped    = false; }
void victronBleSuspend() { s_bleSuspended  = true;  }
void victronBleResume()  { s_bleSuspended  = false; }
bool victronBleSuspended() { return s_bleSuspended; }
bool victronBleScanActive() { return s_supervisorInScan; }

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

    heapDiagMark("ble:before_init");
    LOG_BLE_PF("[ble] NimBLE init  free=%u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    NimBLEDevice::init("");
    s_nimbleUp = true;
    heapDiagMark("ble:nimble_up");
    LOG_BLE_PF("[ble] NimBLE up    free=%u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    // Create the scan handle whenever any passive-scan source is bound:
    // Victron, the BTHome tank, Multiplus, or OpenAir.  The tank, multiplus
    // and openair receivers piggyback on VictronScanCb.
    // Ultimatron is polled over GATT (not scanned), so it does not count.
    if (s_configured || tankIsConfigured() || multiplusIsConfigured() || openairIsConfigured()) {
        s_bleScan = NimBLEDevice::getScan();
        s_bleScan->setScanCallbacks(new VictronScanCb(), true);
        // PASSIVE scan (no scan requests).  All data the supervisor consumes —
        // Victron mfr-data, BTHome tank service-data, Multiplus mfr-data — rides
        // in the ADV payload, never the scan response, so passive loses nothing.
        // It also avoids NimBLE-cpp's active-scan "waiting list": connectable,
        // scannable peers (e.g. the OpenAir A/C) that don't return a scan
        // response within the window get flushed at BLE_GAP_EVENT_DISC_COMPLETE
        // via onResult() on a device whose m_payload can be stale/corrupt,
        // crashing findAdvField() (load fault past the PSRAM top) and rebooting
        // the board every scan cycle.  Passive scan never enters that path.
        s_bleScan->setActiveScan(false);
        // Continuous scan (window == interval): on the JC4880P443C the BLE radio
        // lives on the ESP32-C6 co-processor and time-shares a single RF front
        // end with WiFi. A 50% scan duty cycle (window 80 / interval 160) plus
        // WiFi coexistence left us missing many advertisements, so listen as much
        // as the coex arbiter allows to recover effective RX range. Units = 0.625 ms.
        s_bleScan->setInterval(160);
        s_bleScan->setWindow(160);
        s_bleScan->setMaxResults(0);
    }

    heapDiagMark("ble:scan_ready");
    xTaskCreate(bleSupervisorTask, "ble_sup", 4096, nullptr, 1, &s_bleTaskHandle);
    LOG_BLE_PL("[ble-sup] task created");
}

// Tear NimBLE down to reclaim its internal DRAM (host stack + mbuf pools) —
// used before a self-OTA so the SDIO RX path has headroom.  Waits for the
// supervisor to finish its current scan/poll and exit, stops any residual scan,
// then deinits.  Returns false on timeout so the caller can reboot instead of
// deinit-ing a live stack.  NimBLE comes back via the post-OTA reboot.
//
// deinit(false), NOT (true): deinit(true) deletes the C++ objects AFTER
// nimble_port_deinit() has freed the NPL, and ~NimBLEScan() then calls
// ble_npl_callout_deinit() on that freed NPL → Load-access-fault crash.
// deinit(false) frees the same stack RAM (nimble_port_deinit) but keeps the
// small wrapper objects; we reboot after OTA so they're never reused.
bool bleSupervisorStop() {
    if (!s_nimbleUp) return true;
    s_bleTeardown = true;
    for (int i = 0; i < 250 && s_bleTaskHandle; i++) vTaskDelay(pdMS_TO_TICKS(100)); // ≤25 s
    if (s_bleTaskHandle) {
        LOG_BLE_PL("[ble] teardown timed out — supervisor still running");
        return false;
    }
    if (s_bleScan && s_bleScan->isScanning()) {
        s_bleScan->stop();
        for (int i = 0; i < 30 && s_bleScan->isScanning(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(100));   // let the host task settle before deinit
    s_nimbleUp = false;               // gate any consumer before the stack goes
    LOG_BLE_PF("[ble] deinit free_before=%u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    NimBLEDevice::deinit(false);
    LOG_BLE_PF("[ble] deinit free_after=%u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return true;
}

#else // !ENABLE_BLE

void victronBleInit() {}
void bleSupervisorStart() {}
bool bleSupervisorStop() { return true; }
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
bool victronBleSuspended() { return false; }
bool victronBleScanActive() { return false; }
void victronBleSetAggressive(bool) {}
void victronBleStop()      {}
void victronBleStart()     {}
bool victronLoadConfig(std::string& addr, std::string& key) { (void)addr; (void)key; return false; }
void victronSaveConfig(const std::string& addr, const std::string& key) { (void)addr; (void)key; }
void bleDiscoveryScan(bool, BleDiscoveryCb cb, void* user, uint32_t) { cb(nullptr, 0, user); }

#endif // ENABLE_BLE
