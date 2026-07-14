#include "ws_snapshot.hpp"
#include "webserver.hpp"
#include "p4display.hpp"
#include "p4_ota.hpp"
#include "faultlog.hpp"
#include "crashcatch.hpp"
#include "victronble.hpp"
#include "ultimatronble.hpp"
#include "tankble.hpp"
#include "multiplusble.hpp"
#include "openairble.hpp"
#include "openair_config.hpp"
#include "truma_lin.hpp"
#include "am2301.hpp"
#include "wifi_manager.hpp"
#include "wstunnel.hpp"
#include "mode_controller.hpp"
#include "esp_heap_caps.h"
#include <cmath>
#include <cstdlib>

// Format a Multiplus AC power field for JSON: a plain integer, or "null"
// when the value is the VE.Bus "not available" sentinel (inverter off / not
// reporting), so the web UI shows no reading instead of a bogus number.
static const char* multiPowerJson(int32_t w, char* buf, size_t n) {
    if (w == MULTI_POWER_NA) return "null";
    snprintf(buf, n, "%d", (int)w);
    return buf;
}

void wsSendOtaFrame() {
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

// Scratch buffer size for the connect burst — the largest frame is the crash
// record (~600 B); the snapshot is ~284 B.
static constexpr size_t WS_SNAP_BUF = 640;

void wsOnConnected() {
    P4ControlState cs;
    p4GetControlState(cs);
    WifiStatus ws = wifi_manager_get_status();
    const char* ssid = (ws.connected && ws.ssid[0]) ? ws.ssid : "";
    const char* ip   = (ws.connected && ws.ip[0])   ? ws.ip   : "";

    // Format buffers live in PSRAM, not on the httpd task stack: this handler
    // runs inline on that task and the frames are ~1.2 KB total, which overflowed
    // the (internal-DRAM) stack. PSRAM keeps the stack small AND spares internal
    // DRAM (the OTA self-test heap floor watches it).
    char* buf = (char*)heap_caps_malloc(WS_SNAP_BUF, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (char*)malloc(WS_SNAP_BUF);   // fallback: internal heap
    if (!buf) return;
    snprintf(buf, WS_SNAP_BUF,
             "{\"command\":\"snapshot\","
             "\"settings\":{"
                 "\"/heating\":\"%d\","
                 "\"/fan\":\"%s\","
                 "\"/boiler\":\"%s\","
                 "\"/temp\":\"%.1f\","
                 "\"/ac_mode\":\"%d\","
                 "\"/ac_fan_auto\":\"%d\","
                 "\"/ac_fan_speed\":\"%d\","
                 "\"/ac_flap1\":\"%d\","
                 "\"/ac_flap2\":\"%d\""
             "},"
             "\"status\":{},"
             "\"ssid\":\"%s\","
             "\"ip\":\"%s\","
             "\"energy_idx\":%d,"
             "\"openair\":%d"
             "}",
             cs.heatingOn ? 1 : 0,
             fanIntToStr(cs.fanMode),
             boilerIntToStr(cs.boilerMode),
             cs.roomSetpoint,
             cs.acMode,
             cs.acFanAuto ? 1 : 0,
             cs.acFanSpeed,
             cs.acFlap1,
             cs.acFlap2,
             ssid,
             ip,
             cs.energyIdx,
             openairCfgIsActive() ? 1 : 0);
    wsQueueSend(buf);

    // Push current BLE data so newly-connected browsers don't have to wait
    // for the next change in broadcastBleData() to populate the panels.
    VictronData v = victronGetData();
    snprintf(buf, WS_SNAP_BUF,
             "{\"command\":\"solar\",\"valid\":%s,\"state\":%u,"
             "\"pvW\":%d,\"kWh\":%.2f,\"battV\":%.2f,\"battA\":%.2f}",
             v.valid ? "true" : "false",
             (unsigned)v.state, (int)v.pvW, v.kWhToday, v.battV, v.battA);
    wsQueueSend(buf);

    UltimatronData u = ultimatronGetData();
    snprintf(buf, WS_SNAP_BUF,
             "{\"command\":\"batt\",\"valid\":%s,\"soc\":%u,\"battV\":%.2f,\"battA\":%.2f}",
             u.valid ? "true" : "false", (unsigned)u.soc, u.battV, u.battA);
    wsQueueSend(buf);

    TankData tk = tankGetData();
    snprintf(buf, WS_SNAP_BUF,
             "{\"command\":\"tank\",\"valid\":%s,\"pct\":%u}",
             tk.valid ? "true" : "false", (unsigned)tk.pct);
    wsQueueSend(buf);

    MultiplusData mp = multiplusGetData();
    char mpInW[12], mpOutW[12];
    snprintf(buf, WS_SNAP_BUF,
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

    // OpenAir A/C telemetry (last poll result, if any).
    if (openairIsConfigured()) {
        OpenAirData oa = openairGetData();
        snprintf(buf, WS_SNAP_BUF,
                 "{\"command\":\"ac\",\"valid\":%s,\"conn\":%s,"
                 "\"probe1\":%.1f,\"probe2\":%.1f,"
                 "\"blower_pct\":%d,\"comp_rpm\":%d,\"errors\":%d,\"needpair\":%s}",
                 oa.valid ? "true" : "false", openairConnected() ? "true" : "false",
                 oa.probe1C, oa.probe2C,
                 oa.blowerSpeedPct, oa.compressorSpeedRpm, oa.errors,
                 openairNeedsPair() ? "true" : "false");
        wsQueueSend(buf);
    }

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
    snprintf(buf, WS_SNAP_BUF,
             "{\"command\":\"status\",\"id\":\"/room_temp\",\"value\":\"%s\"}", rt);
    wsQueueSend(buf);
    snprintf(buf, WS_SNAP_BUF,
             "{\"command\":\"status\",\"id\":\"/water_temp\",\"value\":\"%s\"}", wt);
    wsQueueSend(buf);
    snprintf(buf, WS_SNAP_BUF,
             "{\"command\":\"status\",\"id\":\"/water_heating\",\"value\":\"%d\"}",
             lin.waterHeating ? 1 : 0);
    wsQueueSend(buf);
    snprintf(buf, WS_SNAP_BUF,
             "{\"command\":\"status\",\"id\":\"/linok\",\"value\":\"%d\"}",
             lin.linOk ? 1 : 0);
    wsQueueSend(buf);

    // AM2301 external sensor temperature.
    Am2301Data am = am2301GetData();
    char ot[16];
    fmtTemp(am.valid ? am.tempC : NAN, ot, sizeof(ot));
    snprintf(buf, WS_SNAP_BUF,
             "{\"command\":\"status\",\"id\":\"/outdoor_temp\",\"value\":\"%s\"}", ot);
    wsQueueSend(buf);

    // Icon states (BLE + tunnel).  This snapshot must list every source the
    // change-broadcaster (broadcastIconStates) covers — the broadcaster only
    // emits on change, so any state already stable when a client connects is
    // delivered here or never.
    VictronData    vx = victronGetData();
    UltimatronData ux = ultimatronGetData();
    TankData       tx = tankGetData();
    MultiplusData  mx = multiplusGetData();
    OpenAirData    ox = openairGetData();
    int ble = (vx.valid || ux.valid || tx.valid || mx.valid || ox.valid) ? 2
            : (victronIsConfigured() || ultimatronIsConfigured()
               || tankIsConfigured() || multiplusIsConfigured()
               || openairIsConfigured()) ? 1
            : 0;
    snprintf(buf, WS_SNAP_BUF, "{\"command\":\"icon\",\"id\":\"ble\",\"state\":%d}", ble);
    wsQueueSend(buf);
    snprintf(buf, WS_SNAP_BUF, "{\"command\":\"icon\",\"id\":\"tunnel\",\"state\":%d}",
             static_cast<int>(wstunnelUiState()));
    wsQueueSend(buf);

    // OTA status so the page can show an "update available" banner without
    // waiting for the next periodic check.
    wsSendOtaFrame();

    // Last uncontrolled fault (panic/abort/WDT), for the About overlay.
    FaultInfo fi;
    if (faultLogGet(fi)) {
        snprintf(buf, WS_SNAP_BUF,
                 "{\"command\":\"diag\",\"fault\":\"%s\",\"count\":%lu,\"fw\":\"%s\"}",
                 faultReasonName((int)fi.reason), (unsigned long)fi.count, fi.version);
    } else {
        snprintf(buf, WS_SNAP_BUF, "{\"command\":\"diag\",\"fault\":\"\",\"count\":0,\"fw\":\"\"}");
    }
    wsQueueSend(buf);

    // Crash backtrace captured in RTC by the wrapped panic handler (crashcatch).
    // On-demand detail for the About overlay — the addresses are resolved offline
    // with `riscv32-esp-elf-addr2line -e build/truminus.elf <pc/ra/stack…>`.
    CrashInfo ci;
    if (crashCatchGet(ci)) {
        char stk[CrashInfo::STACK_WORDS * 9 + 1];
        int p = 0;
        for (int i = 0; i < CrashInfo::STACK_WORDS && p < (int)sizeof(stk) - 9; i++)
            p += snprintf(stk + p, sizeof(stk) - p, "%08lx ", (unsigned long)ci.stack[i]);
        snprintf(buf, WS_SNAP_BUF,
                 "{\"command\":\"crash\",\"task\":\"%s\",\"reason\":\"%s\",\"core\":%ld,"
                 "\"pc\":\"%08lx\",\"ra\":\"%08lx\",\"sp\":\"%08lx\","
                 "\"mcause\":\"%08lx\",\"mtval\":\"%08lx\",\"stack\":\"%s\"}",
                 ci.task, ci.reason, (long)ci.core,
                 (unsigned long)ci.pc, (unsigned long)ci.ra, (unsigned long)ci.sp,
                 (unsigned long)ci.mcause, (unsigned long)ci.mtval, stk);
        wsQueueSend(buf);
    }

    free(buf);
}
