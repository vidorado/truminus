// BTHome v2 tank-level receiver.
//
// The on-the-bus simulator (TruMinus-HWSim, ESP32-C3) advertises a BTHome
// v2 service-data payload alongside its Victron mfr-data, alternating
// every 2 s.  We hook into the existing Victron scan callback in
// victronble.cpp (NimBLE allows only one scan callback at a time) and
// route ads that carry Service Data UUID 0xFCD2 here.
//
// Frame layout we expect (matches main.c::update_adv_data_bthome on the
// simulator):
//   service_data UUID = 0xFCD2 (BTHome)
//   payload:
//     0x40            device info: v2, unencrypted, no trigger
//     0x00 <seq>      packet ID (uint8) — used only for dedup
//     0x2F <pct>      Moisture (uint8 0..100%) — the tank fill level
//
// Other BTHome tags are ignored.  Encrypted BTHome (device-info byte with
// bit 0 set) is not supported here — we don't ship the AES key.

#include "tankble.hpp"
#include "logs.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "NimBLEDevice.h"
#include "NimBLEAdvertisedDevice.h"

#include <cstring>
#include <cctype>

static const char* TAG = "tankble";

namespace {

// ── Configuration / state ────────────────────────────────────────────────
SemaphoreHandle_t s_dataMux   = nullptr;
TankData          s_data      = { 0, false, 0 };
std::string       s_targetMac;     // uppercase no-separator hex; empty = unbound
uint8_t           s_lastSeq   = 0xFF;
bool              s_haveSeq   = false;

// ── Helpers ──────────────────────────────────────────────────────────────

uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

// Normalise a MAC string to "AABBCCDDEEFF" — uppercase, no separators.
std::string normaliseMac(std::string s) {
    std::string out;
    out.reserve(12);
    for (char c : s) {
        if (c == ':' || c == '-' || c == ' ') continue;
        out.push_back((char)std::toupper((unsigned char)c));
    }
    return out;
}

// Walk BTHome v2 unencrypted payload (after the device-info byte), looking
// for tag 0x2F (moisture uint8) and tag 0x00 (packet ID uint8).  Returns
// true if a moisture sample was found and updates *pct / *seq.
bool parseBthomePayload(const uint8_t* p, int len, uint8_t* pct, uint8_t* seq) {
    // BTHome tag table (only the bits we care about): each tag's value
    // length is implied by the tag itself.  Sizes table for the subset of
    // tags we may encounter — used to skip unknown ones safely.  See
    // https://bthome.io/format/.
    bool gotMoisture = false;
    int i = 0;
    while (i < len) {
        uint8_t tag = p[i++];
        int size;
        switch (tag) {
            case 0x00: size = 1; break;     // packet ID
            case 0x01: size = 1; break;     // battery %
            case 0x02: size = 2; break;     // temperature sint16
            case 0x03: size = 2; break;     // humidity uint16
            case 0x12: size = 2; break;     // CO2 uint16
            case 0x14: size = 2; break;     // moisture uint16
            case 0x2E: size = 1; break;     // humidity uint8
            case 0x2F: size = 1; break;     // moisture uint8  ← we want this
            case 0x4A: size = 2; break;     // voltage uint16
            case 0x51: size = 1; break;     // count uint8
            default:
                // Unknown tag of unknown length — bail rather than mis-parse.
                return gotMoisture;
        }
        if (i + size > len) return gotMoisture;
        if (tag == 0x2F) {
            *pct = p[i];
            gotMoisture = true;
        } else if (tag == 0x00) {
            *seq = p[i];
        }
        i += size;
    }
    return gotMoisture;
}

bool nvsOpenRead(nvs_handle_t* out) {
    return nvs_open("tank", NVS_READONLY, out) == ESP_OK;
}
bool nvsOpenWrite(nvs_handle_t* out) {
    return nvs_open("tank", NVS_READWRITE, out) == ESP_OK;
}

void load_config_internal() {
    nvs_handle_t h;
    if (!nvsOpenRead(&h)) { s_targetMac.clear(); return; }
    char buf[24] = {};
    size_t len = sizeof(buf);
    if (nvs_get_str(h, "addr", buf, &len) == ESP_OK) {
        s_targetMac = normaliseMac(std::string(buf));
    } else {
        s_targetMac.clear();
    }
    nvs_close(h);
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────

void tankBleInit() {
    if (!s_dataMux) s_dataMux = xSemaphoreCreateMutex();
    load_config_internal();
    if (s_targetMac.empty()) {
        LOG_BLE_PL("[tank] not configured (NVS tank/addr empty)");
    } else {
        LOG_BLE_PF("[tank] cfg loaded, target=%s\n", s_targetMac.c_str());
    }
}

void tankBleReloadConfig() {
    load_config_internal();
    if (s_targetMac.empty()) {
        LOG_BLE_PL("[tank] config cleared");
    } else {
        LOG_BLE_PF("[tank] cfg reloaded, target=%s\n", s_targetMac.c_str());
    }
    // Reset dedup so the next ad always counts as a fresh sample.
    s_haveSeq = false;
}

bool tankIsConfigured() {
    return !s_targetMac.empty();
}

TankData tankGetData() {
    TankData out = { 0, false, 0 };
    if (s_dataMux && xSemaphoreTake(s_dataMux, pdMS_TO_TICKS(10)) == pdTRUE) {
        out = s_data;
        xSemaphoreGive(s_dataMux);
    }
    return out;
}

bool tankLoadConfig(std::string& addr) {
    nvs_handle_t h;
    if (!nvsOpenRead(&h)) return false;
    char buf[24] = {};
    size_t len = sizeof(buf);
    bool ok = (nvs_get_str(h, "addr", buf, &len) == ESP_OK);
    nvs_close(h);
    if (ok) addr = buf;
    return ok;
}

void tankSaveConfig(const std::string& addr) {
    nvs_handle_t h;
    if (!nvsOpenWrite(&h)) {
        ESP_LOGE(TAG, "tankSaveConfig: nvs_open failed");
        return;
    }
    if (addr.empty()) {
        nvs_erase_key(h, "addr");
    } else {
        std::string norm = normaliseMac(addr);
        nvs_set_str(h, "addr", norm.c_str());
    }
    nvs_commit(h);
    nvs_close(h);
}

void tankBleHandleAd(const NimBLEAdvertisedDevice* dev) {
    if (s_targetMac.empty()) return;       // not configured — ignore everything
    if (!dev) return;

    // BTHome uses Service Data with 16-bit UUID 0xFCD2.  NimBLE exposes
    // service-data lookup via getServiceData(NimBLEUUID).
    NimBLEUUID bthomeUuid((uint16_t)0xFCD2);
    if (!dev->haveServiceData()) return;
    std::string sd = dev->getServiceData(bthomeUuid);
    if (sd.size() < 2) return;             // device-info + at least 1 tag

    uint8_t devInfo = (uint8_t)sd[0];
    if (devInfo & 0x01) return;            // encrypted → not supported here
    // Bits 5..7 carry the BTHome version; v2 = 0b010 → 0x40.  Accept either
    // exact match or any non-zero version field — we only consume v2 tags
    // so a mismatched version would already fail in parseBthomePayload.
    (void)devInfo;

    // Match source MAC against allow-list before doing anything with the
    // payload — keeps a neighbour's BTHome sensor from spoofing our value.
    std::string devMac = normaliseMac(dev->getAddress().toString());
    if (devMac != s_targetMac) return;

    uint8_t pct = 0, seq = 0;
    if (!parseBthomePayload((const uint8_t*)sd.data() + 1, (int)sd.size() - 1,
                            &pct, &seq)) {
        return;
    }

    // Dedup by packet-id: BTHome advertisers re-emit the same payload many
    // times within a window; only treat a fresh seq as a new sample.
    if (s_haveSeq && seq == s_lastSeq) return;
    s_lastSeq = seq;
    s_haveSeq = true;

    if (s_dataMux && xSemaphoreTake(s_dataMux, portMAX_DELAY) == pdTRUE) {
        s_data.pct    = (pct > 100) ? 100 : pct;
        s_data.valid  = true;
        s_data.lastMs = now_ms();
        xSemaphoreGive(s_dataMux);
    }
    LOG_BLE_PF("[tank] %s pct=%u seq=%u\n", devMac.c_str(),
               (unsigned)s_data.pct, (unsigned)seq);
}
