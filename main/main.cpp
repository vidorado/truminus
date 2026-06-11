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
#include "p4_ota.hpp"
#include "faultlog.hpp"
#include "webserver.hpp"
#include "wstunnel.hpp"
#include "cli.hpp"
#include "truma_lin.hpp"
#include "tankble.hpp"
#include "multiplusble.hpp"
#include "am2301.hpp"
#include "flags.h"
#include "mode_controller.hpp"
#include "ws_command.hpp"
#include "ws_diff.hpp"
#include "heapdiag.hpp"

// LIN bus pins on the JC4880-P4 board — wired to connector J5.
// UART_NUM_1 is fixed inside truma_lin.cpp (UART0 is the debug console).
// J5 connector: ESP TX=GPIO27 (→ transceiver RXD), ESP RX=GPIO26 (← transceiver TXD).
#define LIN_TX_PIN 27
#define LIN_RX_PIN 26

// AM2301 / DHT22 external (outdoor) temperature + humidity sensor DATA line.
// Read via the RMT peripheral (see main/am2301.cpp); needs a pull-up on the
// line (internal one enabled in am2301Start, plus the breakout's own 5.1 kΩ).
#define AM2301_DATA_PIN GPIO_NUM_52
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

// Format a Multiplus AC power field for JSON: a plain integer, or "null"
// when the value is the VE.Bus "not available" sentinel (inverter off / not
// reporting), so the web UI shows no reading instead of a bogus number.
static const char* multiPowerJson(int32_t w, char* buf, size_t n) {
    if (w == MULTI_POWER_NA) return "null";
    snprintf(buf, n, "%d", (int)w);
    return buf;
}

// multiPowerChanged() and the BLE/LIN change-detection predicates live in the
// host-tested ws_diff module; multiPowerJson() (formatting) stays here.

// ── WebSocket command dispatcher ─────────────────────────────────────────
//
// Routes {id,value} frames sent by the browser to the matching p4display
// setter.  Once the LCD state changes, the diff in the main loop emits a
// `setting` broadcast so any other connected tab sees the same change.
//
// Without settings.cpp/trumaframes.cpp ported these are the only commands
// that have any effect; everything else (energy_idx, lang, …) is logged.
static void onWsCommand(const char* id, const char* value) {
    // Parsing/validation is pure and host-tested in ws_command.cpp; this layer
    // only dispatches the result to the p4Set*/OTA setters.
    WsCommand cmd = parseWsCommand(id, value);
    switch (cmd.kind) {
        case WsCmdKind::Heating:    p4SetHeating(cmd.boolVal); break;
        case WsCmdKind::Fan:        if (cmd.valid) p4SetFanMode(cmd.intVal); break;
        case WsCmdKind::Boiler:     if (cmd.valid) p4SetBoilerMode(cmd.intVal); break;
        case WsCmdKind::Temp:       p4SetRoomSetpoint(cmd.floatVal); break;
        case WsCmdKind::EnergyIdx:  p4SetEnergyIdx(cmd.intVal); break;
        case WsCmdKind::OtaCheck:   p4OtaCheckNow(); break;
        case WsCmdKind::OtaInstall: p4OtaInstall(); break;
        case WsCmdKind::OtaCancel:  p4OtaCancel(); break;
        case WsCmdKind::Unknown:    ESP_LOGI(TAG, "ws cmd (unhandled): %s = %s", id, value); break;
        case WsCmdKind::None:       break;
    }
}

// Build and queue the current OTA status frame.  Shared by the on-connect
// snapshot and the live broadcaster so both emit an identical envelope.
static void sendOtaFrame() {
    P4OtaStatus ota;
    p4OtaGetStatus(ota);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"command\":\"ota\",\"checking\":%s,\"available\":%s,\"installing\":%s,"
             "\"progress\":%d,\"current\":\"%s\",\"latest\":\"%s\",\"error\":\"%s\"}",
             ota.checking ? "true" : "false",
             ota.available ? "true" : "false",
             ota.installing ? "true" : "false",
             ota.progress, ota.currentVer, ota.latestVer, ota.error);
    wsQueueSend(buf);
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
             "{\"command\":\"batt\",\"valid\":%s,\"soc\":%u,\"battV\":%.2f,\"battA\":%.2f}",
             u.valid ? "true" : "false", (unsigned)u.soc, u.battV, u.battA);
    wsQueueSend(buf);

    TankData tk = tankGetData();
    snprintf(buf, sizeof(buf),
             "{\"command\":\"tank\",\"valid\":%s,\"pct\":%u}",
             tk.valid ? "true" : "false", (unsigned)tk.pct);
    wsQueueSend(buf);

    MultiplusData mp = multiplusGetData();
    char mpInW[12], mpOutW[12];
    snprintf(buf, sizeof(buf),
             "{\"command\":\"multi\",\"valid\":%s,\"state\":%u,"
             "\"ac_in_w\":%s,\"ac_out_w\":%s,"
             "\"batt_v\":%.2f,\"batt_a\":%.1f,"
             "\"ac_in_state\":%u,\"alarm\":%u,\"soc\":%u}",
             mp.valid ? "true" : "false",
             (unsigned)mp.deviceState,
             multiPowerJson(mp.acInW, mpInW, sizeof(mpInW)),
             multiPowerJson(mp.acOutW, mpOutW, sizeof(mpOutW)),
             std::isnan(mp.battV) ? 0.0 : mp.battV, mp.battA,
             (unsigned)mp.acInState, (unsigned)mp.alarm,
             (unsigned)mp.soc);
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

    // AM2301 external sensor temperature.
    Am2301Data am = am2301GetData();
    char ot[16];
    fmtTemp(am.valid ? am.tempC : NAN, ot, sizeof(ot));
    snprintf(buf, sizeof(buf),
             "{\"command\":\"status\",\"id\":\"/outdoor_temp\",\"value\":\"%s\"}", ot);
    wsQueueSend(buf);

    // Icon states (BLE + tunnel)
    VictronData    vx = victronGetData();
    UltimatronData ux = ultimatronGetData();
    TankData       tx = tankGetData();
    MultiplusData  mx = multiplusGetData();
    int ble = (vx.valid || ux.valid || tx.valid || mx.valid) ? 2
            : (victronIsConfigured() || ultimatronIsConfigured()
               || tankIsConfigured() || multiplusIsConfigured()) ? 1
            : 0;
    snprintf(buf, sizeof(buf), "{\"command\":\"icon\",\"id\":\"ble\",\"state\":%d}", ble);
    wsQueueSend(buf);
    snprintf(buf, sizeof(buf), "{\"command\":\"icon\",\"id\":\"tunnel\",\"state\":%d}",
             static_cast<int>(wstunnelUiState()));
    wsQueueSend(buf);

    // OTA status so the page can show an "update available" banner without
    // waiting for the next periodic check.
    sendOtaFrame();

    // Last uncontrolled fault (panic/abort/WDT), for the About overlay.
    FaultInfo fi;
    if (faultLogGet(fi)) {
        snprintf(buf, sizeof(buf),
                 "{\"command\":\"diag\",\"fault\":\"%s\",\"count\":%lu,\"fw\":\"%s\"}",
                 faultReasonName((int)fi.reason), (unsigned long)fi.count, fi.version);
    } else {
        snprintf(buf, sizeof(buf), "{\"command\":\"diag\",\"fault\":\"\",\"count\":0,\"fw\":\"\"}");
    }
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
    if (!inited || victronChanged(v, prevV)) {
        snprintf(buf, sizeof(buf),
                 "{\"command\":\"solar\",\"valid\":%s,\"state\":%u,"
                 "\"pvW\":%d,\"kWh\":%.2f,\"battV\":%.2f,\"battA\":%.2f}",
                 v.valid ? "true" : "false",
                 (unsigned)v.state, (int)v.pvW, v.kWhToday, v.battV, v.battA);
        wsQueueSend(buf);
        prevV = v;
    }

    UltimatronData u = ultimatronGetData();
    if (!inited || ultimatronChanged(u, prevU)) {
        snprintf(buf, sizeof(buf),
                 "{\"command\":\"batt\",\"valid\":%s,\"soc\":%u,\"battV\":%.2f,\"battA\":%.2f}",
                 u.valid ? "true" : "false", (unsigned)u.soc, u.battV, u.battA);
        wsQueueSend(buf);
        prevU = u;
    }
    inited = true;
}

// Broadcast Multiplus VE.Bus snapshot to WS clients on change.
// Frame shape: {"command":"multi","valid":bool,"state":N,"ac_in_w":N,
//               "ac_out_w":N,"batt_v":F,"batt_a":F,"soc":N,"alarm":N}
static void broadcastMultiplusData() {
    static MultiplusData prev   = {};
    static bool          inited = false;
    char buf[224];

    MultiplusData m = multiplusGetData();
    if (inited && !multiplusChanged(m, prev)) return;
    char mInW[12], mOutW[12];
    snprintf(buf, sizeof(buf),
             "{\"command\":\"multi\",\"valid\":%s,\"state\":%u,"
             "\"ac_in_w\":%s,\"ac_out_w\":%s,"
             "\"batt_v\":%.2f,\"batt_a\":%.1f,"
             "\"ac_in_state\":%u,\"alarm\":%u,\"soc\":%u}",
             m.valid ? "true" : "false",
             (unsigned)m.deviceState,
             multiPowerJson(m.acInW, mInW, sizeof(mInW)),
             multiPowerJson(m.acOutW, mOutW, sizeof(mOutW)),
             std::isnan(m.battV) ? 0.0 : m.battV, m.battA,
             (unsigned)m.acInState, (unsigned)m.alarm,
             (unsigned)m.soc);
    wsQueueSend(buf);
    prev   = m;
    inited = true;
}

// Broadcast tank-level (BTHome) to every connected WS client on change.
// Frame shape mirrors solar/batt: {"command":"tank","valid":bool,"pct":N}.
static void broadcastTankData() {
    static TankData prev   = {};
    static bool     inited = false;
    char buf[96];

    TankData t = tankGetData();
    if (!inited || tankChanged(t, prev)) {
        snprintf(buf, sizeof(buf),
                 "{\"command\":\"tank\",\"valid\":%s,\"pct\":%u}",
                 t.valid ? "true" : "false", (unsigned)t.pct);
        wsQueueSend(buf);
        prev   = t;
        inited = true;
    }
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

    char val[16];

    if (!inited || linTempChanged(lin.roomTemp, prev.roomTemp)) {
        fmtTemp(lin.roomTemp, val, sizeof(val));
        emit("room_temp", val);
    }
    if (!inited || linTempChanged(lin.waterTemp, prev.waterTemp)) {
        fmtTemp(lin.waterTemp, val, sizeof(val));
        emit("water_temp", val);
    }
    if (!inited || lin.waterHeating != prev.waterHeating) {
        emit("water_heating", lin.waterHeating ? "1" : "0");
    }
    if (!inited || lin.linOk != prev.linOk) {
        emit("linok", lin.linOk ? "1" : "0");
    }

    // AM2301 external sensor temperature (not LIN-derived, but shares this
    // change-detected broadcast slot to reuse fmtTemp/tempChanged).
    {
        static float prevOutdoor = NAN;
        Am2301Data   am = am2301GetData();
        float        outdoor = am.valid ? am.tempC : NAN;
        if (!inited || linTempChanged(outdoor, prevOutdoor)) {
            fmtTemp(outdoor, val, sizeof(val));
            emit("outdoor_temp", val);
            prevOutdoor = outdoor;
        }
    }

    prev = lin;
    inited = true;
}

static void broadcastIconStates() {
    static int prevBle = -1;
    static int prevTun = -1;

    VictronData    v  = victronGetData();
    UltimatronData u  = ultimatronGetData();
    TankData       tk = tankGetData();
    MultiplusData  mp = multiplusGetData();
    int ble = (v.valid || u.valid || tk.valid || mp.valid) ? 2
            : (victronIsConfigured() || ultimatronIsConfigured()
               || tankIsConfigured() || multiplusIsConfigured()) ? 1
            : 0;
    int tun = static_cast<int>(wstunnelUiState());

    char buf[80];
    if (ble != prevBle) {
        snprintf(buf, sizeof(buf), "{\"command\":\"icon\",\"id\":\"ble\",\"state\":%d}", ble);
        wsQueueSend(buf);
        prevBle = ble;
    }
    if (tun != prevTun) {
        snprintf(buf, sizeof(buf), "{\"command\":\"icon\",\"id\":\"tunnel\",\"state\":%d}", tun);
        wsQueueSend(buf);
        prevTun = tun;
    }
}

// Live-push OTA status when any user-visible field changes, so a manual
// "Check" result and install progress stream without a reconnect.  Diffing
// is mandatory: otherwise we'd queue an ota frame every 100 ms tick.
static void broadcastOtaStatus() {
    static P4OtaStatus prev;
    static bool inited = false;

    P4OtaStatus ota;
    p4OtaGetStatus(ota);
    bool changed = !inited
                || ota.checking   != prev.checking
                || ota.available  != prev.available
                || ota.installing != prev.installing
                || ota.progress   != prev.progress
                || strcmp(ota.latestVer, prev.latestVer) != 0
                || strcmp(ota.error,     prev.error)     != 0;
    if (changed) {
        sendOtaFrame();
        prev = ota;
        inited = true;
    }
}

static void wsPumpTask(void* /*arg*/) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        P4ControlState cs;
        p4GetControlState(cs);
        broadcastControlChanges(cs);
        broadcastNetInfoChange();
        broadcastBleData();
        broadcastTankData();
        broadcastMultiplusData();
        broadcastLinTemps();
        broadcastIconStates();
        broadcastOtaStatus();
        wsQueueDrain();
        p4OtaBeat(P4OTA_BEAT_WEB);   // liveness for the post-OTA self-test
    }
}

// Background boot: everything that does not need to block the splash.
// Runs in parallel with the splash screen so the user sees pixels as fast
// as bsp_display_start_with_config() returns.  Each step is independent and
// already non-blocking (BLE supervisor self-spawns, wifi_manager_start is
// non-blocking, mountWebFs / startWebServer are fast).
static void bootTask(void* /*arg*/) {
    heapDiagMark("boot:start");

    // WiFi driver init.  ESP-Hosted transport to the C6 co-processor is
    // established here; the C6 OTA check that follows depends on it.
    wifi_manager_init();
    heapDiagMark("hosted+wifi_init");

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
    heapDiagMark("wifi_start");

    // Initialize C6 BT controller via ESP-Hosted RPC before NimBLE starts.
    // This MUST complete before wstunnelInit(): the controller-init RPC and the
    // tunnel's TLS both ride the shared C6 hosted/SDIO transport, and running
    // them concurrently starves the tunnel's websocket client ("Could not lock
    // ws-client") and drops the connection.
    ESP_ERROR_CHECK(esp_hosted_bt_controller_init());
    ESP_ERROR_CHECK(esp_hosted_bt_controller_enable());
    heapDiagMark("bt_controller");

    victronBleInit();
    ultimatronBleInit();
    tankBleInit();
    multiplusBleInit();
    heapDiagMark("ble_inits");
    // NimBLE init + supervisor task creation is deferred to the end of bootTask
    // (after boot:complete) so it does not interleave with the sequential
    // bring-up marks below — and so its heap peak does not overlap the web /
    // tunnel bring-up (consistent with the §16 "sequence, don't stack peaks"
    // rule). The supervisor task itself already waits for WiFi before scanning.

    // Web assets live on a LittleFS partition flashed from <project>/data/.
    if (mountWebFs() != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS mount failed — run 'idf.py littlefs-flash-littlefs'");
    }
    heapDiagMark("littlefs");
    startWebServer(onWsCommand, onWsConnected);
    heapDiagMark("webserver");

    // WebSocket reverse tunnel — exposes the local HTTP server through CGNAT
    // via a Plesk-hosted Node.js bridge.  Spawns its own task; respects the
    // "tunnel/enabled" NVS flag.  On a freshly-OTA'd image the tunnel's TLS
    // handshake is deferred (p4OtaPendingVerify) so its internal-DRAM/SRAM
    // spike doesn't coincide with WiFi/BLE bring-up during the self-test
    // heap-floor window; the OTA self-test resumes it once the image is valid.
    wstunnelInit(p4OtaPendingVerify());
    heapDiagMark("wstunnel_init");

    // WS pump runs at 100 ms cadence so touch inputs on the LCD reach
    // connected browsers in ≤100 ms.  Lower than the main loop's 1 s tick.
    xTaskCreate(wsPumpTask, "ws_pump", 4096, nullptr, 3, nullptr);
    heapDiagMark("ws_pump");

    // Serial REPL on USB-Serial-JTAG — provisions WiFi / Victron /
    // Ultimatron / tunnel credentials while the LCD settings screen is
    // unavailable.  Commands: `wifi`, `victron`, `ultimatron`, `tunnel`,
    // `show`, `help`.
    cliStart();
    heapDiagMark("cli");

    // AM2301 / DHT22 external temperature sensor on GPIO52 (RMT-based reader,
    // 30 s cadence).  Populates d.outdoorTemp in the main loop and broadcasts
    // as the WS `outdoor_temp` status id.
    am2301Start(AM2301_DATA_PIN);

    // LIN scheduler — emulates the CP-Plus D control unit on UART1.
    // Pinned to Core 0 (legacy convention: blocking serial off the LVGL core).
    trumaLinStart(LIN_TX_PIN, LIN_RX_PIN);
    heapDiagMark("am2301+lin");

    // Self-OTA: periodic GitHub release check + post-OTA self-test/rollback.
    // Spawned last so all the subsystems it watches (tunnel/LIN/BLE/web) are
    // already up before the self-test starts sampling their heartbeats.
    p4OtaStart();

    heapDiagMark("boot:complete");
    ESP_LOGI("boot", "background boot complete (heap=%lu)",
             (unsigned long)esp_get_free_heap_size());

    // Bring up NimBLE + the BLE supervisor last, in its own task, so its heap
    // peak lands after every other subsystem has settled and its bring-up marks
    // do not interleave with bootTask's sequential marks above.
    xTaskCreate([](void*) {
        bleSupervisorStart();
        vTaskDelete(nullptr);
    }, "ble_start", 6144, nullptr, 1, nullptr);

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

    // Capture why we last reset (panic/abort/WDT vs controlled/power) before
    // anything else can reboot the chip.
    faultLogInit();

    // Default event loop and netif — required by WiFi.
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    // Apply per-TAG log levels (silences web/wstunnel/ledc/httpd by default;
    // see main/flags.h to re-enable or tune).  Must run before any other
    // subsystem emits its first message.
    flags_apply_log_levels();

    // Pick up persisted UI language before the display builds any labels.
    loadLanguage();

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
    // (No boot status here: the status bar belongs to build_main_screen, which
    // runs after the splash — setting it now would target a not-yet-built label.)

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
        p4OtaBeat(P4OTA_BEAT_LOOP);   // liveness for the post-OTA self-test

        // Steady-state internal-DRAM probe every 30 s — the `min` field is the
        // worst-case ever seen, i.e. how close we run to the OTA heap floor.
        if (iter % 30 == 0) heapDiagMark("steady");

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
            d.solar.kWhToday = vd.kWhToday;
        } else {
            d.solar.valid = false;
        }

        if (ud.valid) {
            d.batt.valid    = true;
            d.batt.soc      = (int)ud.soc;
            d.batt.voltageV = ud.battV;
            d.batt.currentA = ud.battA;
        } else {
            d.batt.valid = false;
        }

        // Fresh-water tank — BTHome sensor (TruMinus-HWSim today,
        // dedicated C3 once the firmware is forked out).
        TankData td = tankGetData();
        d.tank.valid = td.valid;
        d.tank.pct   = td.pct;

        // Multiplus VE.Bus snapshot — read-only (control needs GATT).
        MultiplusData mp = multiplusGetData();
        d.multi.valid       = mp.valid;
        d.multi.deviceState = mp.deviceState;
        d.multi.stateName   = multiplusStateName(mp.deviceState);
        d.multi.acInW       = mp.acInW;
        d.multi.acOutW      = mp.acOutW;
        d.multi.battV       = mp.battV;
        d.multi.battA       = mp.battA;
        d.multi.soc         = mp.soc;
        d.multi.alarm       = mp.alarm;
        d.multi.acInState   = mp.acInState;

        // LIN snapshot → room/water temp + LIN-ok dot.
        TrumaLinSnapshot lin;
        trumaLinGetSnapshot(lin);
        d.linOk = lin.linOk;
        d.roomTemp  = lin.roomTemp;   // already NAN when no valid frame yet
        d.waterTemp = lin.waterTemp;

        // AM2301 external sensor → outdoor temp (NAN until first valid read).
        Am2301Data am = am2301GetData();
        d.outdoorTemp = am.valid ? am.tempC : NAN;

        // Status line: switch to "No LIN bus" once we've given the bus a few
        // seconds to come up.  Mirrors the web UI's red "No LIN bus" message.
        // Only push on transitions so the slot remains free for ad-hoc messages.
        {
            static int  prevStatus = -1;   // -1=unknown, 0=ok/empty, 1=no-LIN
            int  newStatus = (iter >= 10 && !lin.linOk) ? 1 : 0;
            if (newStatus != prevStatus) {
                if (newStatus == 1)        p4DisplaySetStatus(t(TK::STATUS_NO_LIN), true);
                else if (prevStatus != 0)  p4DisplaySetStatus("", false);  // clear boot "Iniciando…" / No-LIN
                prevStatus = newStatus;
            }
        }

        // Tunnel state → topbar cloud icon (grey/blinking/blue/red).
        p4SetTunnelState(static_cast<uint8_t>(wstunnelUiState()));

        // Firmware-update reminder: topbar icon + one-shot "update now?" modal.
        p4SetUpdateAvailable(p4OtaNotify());
        char ota_cur[32], ota_latest[32];
        if (p4OtaPromptPending(ota_cur, sizeof(ota_cur),
                               ota_latest, sizeof(ota_latest))) {
            if (p4DisplayShowUpdatePrompt(ota_cur, ota_latest))
                p4OtaMarkPrompted();
        }

        d.bleState = (vd.valid || ud.valid || td.valid || mp.valid) ? 2
                   : (victronIsConfigured() || ultimatronIsConfigured()
                      || tankIsConfigured() || multiplusIsConfigured()) ? 1
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
