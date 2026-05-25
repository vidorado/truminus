#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "globals.hpp"
#include "logs.hpp"
#include "i18n.hpp"
#include "p4display.hpp"
#include "wifi_manager.hpp"
#include "victronble.hpp"
#include "ultimatronble.hpp"
#include "c6_ota.hpp"
#include "webserver.hpp"
#include "wstunnel.hpp"
#include "cli.hpp"
#include "truma_lin.hpp"
#include "flags.h"

// LIN bus pins on the JC4880-P4 board — wired to connector J5.
// UART_NUM_1 is fixed inside truma_lin.cpp (UART0 is the debug console).
// J5 connector: ESP TX=GPIO27 (→ transceiver RXD), ESP RX=GPIO26 (← transceiver TXD).
#define LIN_TX_PIN 27
#define LIN_RX_PIN 26
#include "esp_hosted_host_fw_ver.h"
extern "C" {
#include "esp_hosted_misc.h"
}
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <strings.h>            // strcasecmp
#include <stdio.h>

static const char* TAG = "main";

// ── String ↔ int helpers for the wire protocol ───────────────────────────
//
// The web UI uses string values for fan/boiler ("eco"/"high"/"1".."10"/etc.);
// the LCD state uses ints.  These two helpers are the single source of truth
// for the mapping and are used both when applying remote commands and when
// broadcasting LCD-originated changes back to the web.

// Fan: 0=off, 1=eco, 2=high, 3..12 = level 1..10.  Unknown strings → -1.
static int fanStrToInt(const char* v) {
    if (!v) return -1;
    if (strcmp(v, "off")  == 0) return 0;
    if (strcmp(v, "eco")  == 0) return 1;
    if (strcmp(v, "high") == 0) return 2;
    int n = atoi(v);
    if (n >= 1 && n <= 10) return n + 2;
    return -1;
}
static const char* fanIntToStr(int m) {
    static char lvl[4];
    switch (m) {
        case 0:  return "off";
        case 1:  return "eco";
        case 2:  return "high";
        default:
            if (m >= 3 && m <= 12) { snprintf(lvl, sizeof(lvl), "%d", m - 2); return lvl; }
            return "off";
    }
}

// Boiler: 0=off, 1=eco, 2=high, 3=boost.
static int boilerStrToInt(const char* v) {
    if (!v) return -1;
    if (strcmp(v, "off")   == 0) return 0;
    if (strcmp(v, "eco")   == 0) return 1;
    if (strcmp(v, "high")  == 0) return 2;
    if (strcmp(v, "boost") == 0) return 3;
    return -1;
}
static const char* boilerIntToStr(int m) {
    static const char* T[4] = { "off", "eco", "high", "boost" };
    return (m >= 0 && m < 4) ? T[m] : "off";
}

// ── WebSocket command dispatcher ─────────────────────────────────────────
//
// Routes {id,value} frames sent by the browser to the matching p4display
// setter.  Once the LCD state changes, the diff in the main loop emits a
// `setting` broadcast so any other connected tab sees the same change.
//
// Without settings.cpp/trumaframes.cpp ported these are the only commands
// that have any effect; everything else (energy_idx, lang, …) is logged.
static void onWsCommand(const char* id, const char* value) {
    if (!id || !value) return;
    // Strip leading slash — the browser sends "/heating" but the route table
    // here uses bare names.
    const char* k = (id[0] == '/') ? id + 1 : id;

    if (strcmp(k, "heating") == 0) {
        p4SetHeating(strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0);
    } else if (strcmp(k, "fan") == 0) {
        int m = fanStrToInt(value);
        if (m >= 0) p4SetFanMode(m);
    } else if (strcmp(k, "boiler") == 0) {
        int m = boilerStrToInt(value);
        if (m >= 0) p4SetBoilerMode(m);
    } else if (strcmp(k, "temp") == 0) {
        p4SetRoomSetpoint(strtof(value, nullptr));
    } else if (strcmp(k, "energy_idx") == 0) {
        p4SetEnergyIdx(atoi(value));
    } else {
        ESP_LOGI(TAG, "ws cmd (unhandled): %s = %s", id, value);
    }
}

// Browser just connected and sent "settings": push a snapshot of the
// current control state so the page reflects the LCD without waiting for
// the next user interaction.
static void onWsConnected() {
    P4ControlState cs;
    p4GetControlState(cs);
    WifiStatus ws = wifi_manager_get_status();
    const char* ssid = (ws.connected && ws.ssid[0]) ? ws.ssid : "";
    const char* ip   = (ws.connected && ws.ip[0])   ? ws.ip   : "";

    char buf[352];
    snprintf(buf, sizeof(buf),
             "{\"command\":\"snapshot\","
             "\"settings\":{"
                 "\"/heating\":\"%d\","
                 "\"/fan\":\"%s\","
                 "\"/boiler\":\"%s\","
                 "\"/temp\":\"%.1f\""
             "},"
             "\"status\":{},"
             "\"ssid\":\"%s\","
             "\"ip\":\"%s\","
             "\"energy_idx\":%d"
             "}",
             cs.heatingOn ? 1 : 0,
             fanIntToStr(cs.fanMode),
             boilerIntToStr(cs.boilerMode),
             cs.roomSetpoint,
             ssid,
             ip,
             cs.energyIdx);
    wsQueueSend(buf);

    // Push current BLE data so newly-connected browsers don't have to wait
    // for the next change in broadcastBleData() to populate the panels.
    VictronData v = victronGetData();
    snprintf(buf, sizeof(buf),
             "{\"command\":\"solar\",\"valid\":%s,\"state\":%u,"
             "\"pvW\":%d,\"kWh\":%.2f,\"battV\":%.2f,\"battA\":%.2f}",
             v.valid ? "true" : "false",
             (unsigned)v.state, (int)v.pvW, v.kWhToday, v.battV, v.battA);
    wsQueueSend(buf);

    UltimatronData u = ultimatronGetData();
    snprintf(buf, sizeof(buf),
             "{\"command\":\"batt\",\"valid\":%s,\"soc\":%u,\"battV\":%.2f}",
             u.valid ? "true" : "false", (unsigned)u.soc, u.battV);
    wsQueueSend(buf);

    // Push current LIN snapshot so freshly-loaded pages get room/water temp
    // without waiting for the next change in broadcastLinTemps().
    TrumaLinSnapshot lin;
    trumaLinGetSnapshot(lin);
    auto fmtTemp = [](float t, char* out, size_t n) {
        if (!std::isfinite(t) || t <= -200.0f) snprintf(out, n, "-273");
        else                              snprintf(out, n, "%.1f", t);
    };
    char rt[16], wt[16];
    fmtTemp(lin.roomTemp,  rt, sizeof(rt));
    fmtTemp(lin.waterTemp, wt, sizeof(wt));
    snprintf(buf, sizeof(buf),
             "{\"command\":\"status\",\"id\":\"/room_temp\",\"value\":\"%s\"}", rt);
    wsQueueSend(buf);
    snprintf(buf, sizeof(buf),
             "{\"command\":\"status\",\"id\":\"/water_temp\",\"value\":\"%s\"}", wt);
    wsQueueSend(buf);
    snprintf(buf, sizeof(buf),
             "{\"command\":\"status\",\"id\":\"/water_heating\",\"value\":\"%d\"}",
             lin.waterHeating ? 1 : 0);
    wsQueueSend(buf);
    snprintf(buf, sizeof(buf),
             "{\"command\":\"status\",\"id\":\"/linok\",\"value\":\"%d\"}",
             lin.linOk ? 1 : 0);
    wsQueueSend(buf);
}

// ── Broadcast LCD-originated changes ─────────────────────────────────────
// Called once per main-loop tick.  Diffs the current control state against
// the previous snapshot; for each field that changed emits a single WS
// `setting` frame so every connected browser tab updates without polling.
static void broadcastControlChanges(const P4ControlState& cs) {
    static P4ControlState prev = {};
    static bool          inited = false;
    char buf[160];

    auto emit = [&](const char* id, const char* value) {
        snprintf(buf, sizeof(buf),
                 "{\"command\":\"setting\",\"id\":\"/%s\",\"value\":\"%s\"}",
                 id, value);
        wsQueueSend(buf);
    };

    if (!inited || cs.heatingOn != prev.heatingOn) {
        emit("heating", cs.heatingOn ? "1" : "0");
    }
    if (!inited || cs.fanMode != prev.fanMode) {
        emit("fan", fanIntToStr(cs.fanMode));
    }
    if (!inited || cs.boilerMode != prev.boilerMode) {
        emit("boiler", boilerIntToStr(cs.boilerMode));
    }
    if (!inited || cs.energyIdx != prev.energyIdx) {
        char v[8]; snprintf(v, sizeof(v), "%d", cs.energyIdx);
        emit("energy_idx", v);
    }
    if (!inited || cs.roomSetpoint != prev.roomSetpoint) {
        char v[8]; snprintf(v, sizeof(v), "%.1f", cs.roomSetpoint);
        emit("temp", v);
    }
    prev   = cs;
    inited = true;
}

// Dedicated WebSocket pump task.  Polls the LCD control state and drains
// the broadcast queue every 100 ms so user input on the touch screen
// reaches connected browsers in ≤100 ms.  The main loop runs at 1 s, which
// is fine for display refresh and BLE/WiFi status polling but felt
// sluggish over the WS — keep that loop coarse and let this one carry the
// WS latency budget.
// Broadcast SSID changes to connected browsers (footer shows "ssid / ip").
// Same diff-based scheme as broadcastControlChanges: emit once on change.
static void broadcastNetInfoChange() {
    static char prev_ssid[33] = "";
    static char prev_ip[16]   = "";
    static bool inited        = false;
    WifiStatus ws = wifi_manager_get_status();
    const char* ssid = (ws.connected && ws.ssid[0]) ? ws.ssid : "";
    const char* ip   = (ws.connected && ws.ip[0])   ? ws.ip   : "";
    bool ssid_chg = strcmp(ssid, prev_ssid) != 0;
    bool ip_chg   = strcmp(ip,   prev_ip)   != 0;
    if (inited && !ssid_chg && !ip_chg) return;
    char buf[160];
    if (ssid_chg || !inited) {
        snprintf(buf, sizeof(buf),
                 "{\"command\":\"status\",\"id\":\"/ssid\",\"value\":\"%s\"}", ssid);
        wsQueueSend(buf);
        strncpy(prev_ssid, ssid, sizeof(prev_ssid) - 1);
        prev_ssid[sizeof(prev_ssid) - 1] = '\0';
    }
    if (ip_chg || !inited) {
        snprintf(buf, sizeof(buf),
                 "{\"command\":\"status\",\"id\":\"/ip\",\"value\":\"%s\"}", ip);
        wsQueueSend(buf);
        strncpy(prev_ip, ip, sizeof(prev_ip) - 1);
        prev_ip[sizeof(prev_ip) - 1] = '\0';
    }
    inited = true;
}

// Broadcast Victron + Ultimatron BLE data on change.  Frames match the
// shape applySolar / applyBatt expect in data/script.js.
static void broadcastBleData() {
    static VictronData    prevV = {};
    static UltimatronData prevU = {};
    static bool           inited = false;
    char buf[256];

    VictronData v = victronGetData();
    bool vChanged = !inited
                    || v.valid != prevV.valid
                    || (v.valid && (v.state != prevV.state
                                    || fabsf(v.battV - prevV.battV)  > 0.05f
                                    || fabsf(v.battA - prevV.battA)  > 0.05f
                                    || fabsf(v.pvW   - prevV.pvW)    > 0.5f
                                    || fabsf(v.kWhToday - prevV.kWhToday) > 0.01f));
    if (vChanged) {
        snprintf(buf, sizeof(buf),
                 "{\"command\":\"solar\",\"valid\":%s,\"state\":%u,"
                 "\"pvW\":%d,\"kWh\":%.2f,\"battV\":%.2f,\"battA\":%.2f}",
                 v.valid ? "true" : "false",
                 (unsigned)v.state, (int)v.pvW, v.kWhToday, v.battV, v.battA);
        wsQueueSend(buf);
        prevV = v;
    }

    UltimatronData u = ultimatronGetData();
    bool uChanged = !inited
                    || u.valid != prevU.valid
                    || (u.valid && (u.soc != prevU.soc
                                    || fabsf(u.battV - prevU.battV) > 0.05f));
    if (uChanged) {
        snprintf(buf, sizeof(buf),
                 "{\"command\":\"batt\",\"valid\":%s,\"soc\":%u,\"battV\":%.2f}",
                 u.valid ? "true" : "false", (unsigned)u.soc, u.battV);
        wsQueueSend(buf);
        prevU = u;
    }
    inited = true;
}

// Broadcast LIN-derived values (room/water temp, water-heating flag, LIN-ok)
// to every connected WS client.  Mirrors the room_temp / water_temp /
// water_heating / linok ids handled by data/script.js::applyStatus.
static void broadcastLinTemps() {
    static TrumaLinSnapshot prev = {};
    static bool             inited = false;
    char buf[96];

    TrumaLinSnapshot lin;
    trumaLinGetSnapshot(lin);

    auto emit = [&](const char* id, const char* value) {
        snprintf(buf, sizeof(buf),
                 "{\"command\":\"status\",\"id\":\"/%s\",\"value\":\"%s\"}",
                 id, value);
        wsQueueSend(buf);
    };

    auto fmtTemp = [](float t, char* out, size_t n) {
        if (!std::isfinite(t) || t <= -200.0f) snprintf(out, n, "-273");
        else                              snprintf(out, n, "%.1f", t);
    };

    auto tempChanged = [](float a, float b) {
        bool aBad = !std::isfinite(a) || a <= -200.0f;
        bool bBad = !std::isfinite(b) || b <= -200.0f;
        if (aBad != bBad) return true;
        if (aBad && bBad) return false;
        return fabsf(a - b) > 0.05f;
    };

    char val[16];

    if (!inited || tempChanged(lin.roomTemp, prev.roomTemp)) {
        fmtTemp(lin.roomTemp, val, sizeof(val));
        emit("room_temp", val);
    }
    if (!inited || tempChanged(lin.waterTemp, prev.waterTemp)) {
        fmtTemp(lin.waterTemp, val, sizeof(val));
        emit("water_temp", val);
    }
    if (!inited || lin.waterHeating != prev.waterHeating) {
        emit("water_heating", lin.waterHeating ? "1" : "0");
    }
    if (!inited || lin.linOk != prev.linOk) {
        emit("linok", lin.linOk ? "1" : "0");
    }

    prev = lin;
    inited = true;
}

static void wsPumpTask(void* /*arg*/) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        P4ControlState cs;
        p4GetControlState(cs);
        broadcastControlChanges(cs);
        broadcastNetInfoChange();
        broadcastBleData();
        broadcastLinTemps();
        wsQueueDrain();
    }
}

// Background boot: everything that does not need to block the splash.
// Runs in parallel with the splash screen so the user sees pixels as fast
// as bsp_display_start_with_config() returns.  Each step is independent and
// already non-blocking (BLE supervisor self-spawns, wifi_manager_start is
// non-blocking, mountWebFs / startWebServer are fast).
static void bootTask(void* /*arg*/) {
    // WiFi driver init.  ESP-Hosted transport to the C6 co-processor is
    // established here; the C6 OTA check that follows depends on it.
    wifi_manager_init();

    // C6 co-processor OTA: if the embedded slave firmware version differs
    // from the host ESP-Hosted library (major.minor), reflash the C6 via
    // SDIO and restart.  The OTA screen overrides the splash; the device
    // reboots when it completes, so we never return.
    {
        char slave_ver[16] = "?";
        if (c6OtaNeeded(slave_ver, sizeof(slave_ver))) {
            char host_ver[16];
            snprintf(host_ver, sizeof(host_ver), "%d.%d.%d",
                     ESP_HOSTED_VERSION_MAJOR_1,
                     ESP_HOSTED_VERSION_MINOR_1,
                     ESP_HOSTED_VERSION_PATCH_1);
            p4DisplayShowOtaScreen(slave_ver, host_ver);
            if (c6OtaPerform(p4DisplaySetOtaProgress)) {
                p4DisplaySetOtaProgress(100);
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            }
        }
    }

    wifi_manager_start();

    // Initialize C6 BT controller via ESP-Hosted RPC before NimBLE starts.
    ESP_ERROR_CHECK(esp_hosted_bt_controller_init());
    ESP_ERROR_CHECK(esp_hosted_bt_controller_enable());

    victronBleInit();
    ultimatronBleInit();
    xTaskCreate([](void*) {
        bleSupervisorStart();
        vTaskDelete(nullptr);
    }, "ble_start", 6144, nullptr, 1, nullptr);

    // Web assets live on a LittleFS partition flashed from <project>/data/.
    if (mountWebFs() != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS mount failed — run 'idf.py littlefs-flash-littlefs'");
    }
    startWebServer(onWsCommand, onWsConnected);

    // WebSocket reverse tunnel — exposes the local HTTP server through CGNAT
    // via a Plesk-hosted Node.js bridge.  Spawns its own task; respects the
    // "tunnel/enabled" NVS flag.
    wstunnelInit();

    // WS pump runs at 100 ms cadence so touch inputs on the LCD reach
    // connected browsers in ≤100 ms.  Lower than the main loop's 1 s tick.
    xTaskCreate(wsPumpTask, "ws_pump", 4096, nullptr, 3, nullptr);

    // Serial REPL on USB-Serial-JTAG — provisions WiFi / Victron /
    // Ultimatron / tunnel credentials while the LCD settings screen is
    // unavailable.  Commands: `wifi`, `victron`, `ultimatron`, `tunnel`,
    // `show`, `help`.
    cliStart();

    // LIN scheduler — emulates the CP-Plus D control unit on UART1.
    // Pinned to Core 0 (legacy convention: blocking serial off the LVGL core).
    trumaLinStart(LIN_TX_PIN, LIN_RX_PIN);

    ESP_LOGI("boot", "background boot complete (heap=%lu)",
             (unsigned long)esp_get_free_heap_size());
    vTaskDelete(nullptr);
}

extern "C" void app_main(void)
{
    // NVS first — WiFi/BLE/settings all need it.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Default event loop and netif — required by WiFi.
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    // Apply per-TAG log levels (silences web/wstunnel/ledc/httpd by default;
    // see main/flags.h to re-enable or tune).  Must run before any other
    // subsystem emits its first message.
    flags_apply_log_levels();

    ESP_LOGI(TAG, "TruMinus P4 — starting (heap=%lu)",
             (unsigned long)esp_get_free_heap_size());

    // Display first so the user sees pixels as soon as the panel is up.
    // Everything below this point runs in the background to keep the
    // splash visible without artificial padding: p4DisplayUpdate() in the
    // main loop enforces a 2 s minimum splash; if the background boot
    // takes longer, the splash naturally stays until it finishes (the
    // main screen still renders, just with empty fields that fill in as
    // each subsystem comes online).
    p4DisplayInit();
    p4DisplaySetStatus(t(TK::STATUS_INIT));

    // Spawn the heavy init in a background task so the splash is on screen
    // immediately and the LVGL refresh task is not starved by app_main.
    xTaskCreate(bootTask, "boot", 6144, nullptr, 5, nullptr);

    P4DisplayData d = {};
    d.roomTemp     = NAN;
    d.waterTemp    = NAN;
    d.outdoorTemp  = NAN;
    d.roomSetpoint = 20.0f;

    p4DisplayUpdate(d);

    uint32_t iter = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        iter++;

        // (WS drain + LCD-change broadcast run in wsPumpTask at 100 ms.)

        // WiFi status
        WifiStatus ws = wifi_manager_get_status();
        d.wifiOk = ws.connected;
        d.ssid   = ws.connected ? ws.ssid : nullptr;
        d.ip     = ws.connected ? ws.ip   : nullptr;

        // Connection info goes to lbl_conn (bottom line) via p4DisplayUpdate.
        // Top status line is for actions/errors only; don't overwrite it here.

        // BLE / solar data
        VictronData    vd = victronGetData();
        UltimatronData ud = ultimatronGetData();

        if (vd.valid) {
            d.solar.valid   = true;
            // Map numeric state to English string (translate_solar_status handles i18n)
            static const char* STATE_STR[] = {
                "Off", "", "Fault", "Bulk", "Absorption", "Float"
            };
            uint8_t st = vd.state;
            d.solar.status   = (st < 6) ? STATE_STR[st] : "Off";
            d.solar.voltageV = vd.battV;
            d.solar.currentA = vd.battA;
            d.solar.powerW   = (int)vd.pvW;
        } else {
            d.solar.valid = false;
        }

        if (ud.valid) {
            d.batt.valid    = true;
            d.batt.soc      = (int)ud.soc;
            d.batt.voltageV = ud.battV;
        } else {
            d.batt.valid = false;
        }

        // LIN snapshot → room/water temp + LIN-ok dot.
        TrumaLinSnapshot lin;
        trumaLinGetSnapshot(lin);
        d.linOk = lin.linOk;
        d.roomTemp  = lin.roomTemp;   // already NAN when no valid frame yet
        d.waterTemp = lin.waterTemp;

        // Tunnel state → topbar cloud icon (grey/blinking/blue/red).
        p4SetTunnelState(static_cast<uint8_t>(wstunnelUiState()));

        d.bleState = (vd.valid || ud.valid) ? 2
                   : (victronIsConfigured() || ultimatronIsConfigured()) ? 1
                   : 0;

        // Dummy overrides (in dummy_flags.h)
#ifdef ENABLE_BOILER_DUMMY
        d.waterTemp = 52.0f + 5.0f * sinf((float)iter * 0.3f);
#endif
#ifdef ENABLE_TEMP_DUMMY
        d.roomTemp    = 20.5f + 0.5f * sinf((float)iter * 0.2f);
        d.outdoorTemp = 12.0f + 2.0f * sinf((float)iter * 0.15f);
#endif

        // Reflect interactive button state so user presses are not overwritten.
        P4ControlState cs;
        p4GetControlState(cs);
        d.heatingOn    = cs.heatingOn;
        d.fanMode      = cs.fanMode;
        d.boilerMode   = cs.boilerMode;
        d.energyIdx    = cs.energyIdx;
        d.roomSetpoint = cs.roomSetpoint;

        p4DisplayUpdate(d);
    }
}
