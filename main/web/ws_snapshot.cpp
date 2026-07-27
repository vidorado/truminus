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
#include "ws_frames.hpp"
#include "ble_status.hpp"
#include "esp_heap_caps.h"
#include <cmath>
#include <cstdlib>

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
    // for the next change in the broadcaster to populate the panels.
    wsFmtSolar(buf, WS_SNAP_BUF, victronGetData());
    wsQueueSend(buf);

    wsFmtBatt(buf, WS_SNAP_BUF, ultimatronGetData());
    wsQueueSend(buf);

    wsFmtTank(buf, WS_SNAP_BUF, tankGetData());
    wsQueueSend(buf);

    wsFmtMulti(buf, WS_SNAP_BUF, multiplusGetData());
    wsQueueSend(buf);

    // OpenAir A/C telemetry (last poll result, if any).
    if (openairIsConfigured()) {
        wsFmtAc(buf, WS_SNAP_BUF, openairGetData(),
                openairConnected(), openairNeedsPair());
        wsQueueSend(buf);
    }

    // Push current LIN snapshot so freshly-loaded pages get room/water temp
    // without waiting for the next change in the broadcaster.
    // Seeded to "no data" so a failed lock take sends honest placeholders
    // rather than garbage; the next change broadcast corrects them.
    TrumaLinSnapshot lin = { false, NAN, NAN, false, 0, 0, 0, 0 };
    trumaLinGetSnapshot(lin);
    char val[16];
    wsFmtTemp(val, sizeof(val), lin.roomTemp);
    wsFmtStatus(buf, WS_SNAP_BUF, "room_temp", val);
    wsQueueSend(buf);
    wsFmtTemp(val, sizeof(val), lin.waterTemp);
    wsFmtStatus(buf, WS_SNAP_BUF, "water_temp", val);
    wsQueueSend(buf);
    wsFmtStatus(buf, WS_SNAP_BUF, "water_heating", lin.waterHeating ? "1" : "0");
    wsQueueSend(buf);
    wsFmtStatus(buf, WS_SNAP_BUF, "linok", lin.linOk ? "1" : "0");
    wsQueueSend(buf);

    // AM2301 external sensor temperature.
    Am2301Data am = am2301GetData();
    wsFmtTemp(val, sizeof(val), am.valid ? am.tempC : NAN);
    wsFmtStatus(buf, WS_SNAP_BUF, "outdoor_temp", val);
    wsQueueSend(buf);

    // Icon states (BLE + tunnel).  This snapshot must cover every icon the
    // change-broadcaster (broadcastIconStates) emits — the broadcaster only
    // fires on change, so any state already stable when a client connects is
    // delivered here or never.
    wsFmtIcon(buf, WS_SNAP_BUF, "ble", bleIconState());
    wsQueueSend(buf);
    wsFmtIcon(buf, WS_SNAP_BUF, "tunnel", static_cast<int>(wstunnelUiState()));
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
