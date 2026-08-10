#include "ws_broadcaster.hpp"
#include "ws_snapshot.hpp"
#include "webserver.hpp"
#include "p4display.hpp"
#include "p4_ota.hpp"
#include "victronble.hpp"
#include "ultimatronble.hpp"
#include "tankble.hpp"
#include "multiplusble.hpp"
#include "openairble.hpp"
#include "openair_config.hpp"
#include "truma_lin.hpp"
#include "lin_codec.hpp"
#include "am2301.hpp"
#include "wifi_manager.hpp"
#include "wstunnel.hpp"
#include "mode_controller.hpp"
#include "ws_diff.hpp"
#include "ws_frames.hpp"
#include "ble_status.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <cmath>
#include <cstring>

// Set when the A/C control state was just adopted from unit telemetry (not a
// user action). broadcastControlChanges still emits the changed fields so the UI
// mirrors the unit, but must NOT echo a command back (that would re-apply it and
// make the unit beep). Consumed and cleared on the next broadcast.
static bool s_acAdoptedFromUnit = false;

// Timestamp (ms) of the last user-originated A/C command. Adoption is suppressed
// for AC_SETTLE_MS afterwards so it does not revert the user's change back to
// stale telemetry: a command takes a full connect→write→readback (several
// seconds), during which the unit still reports the OLD state. The window must
// outlast that round-trip; when it expires the readback telemetry already
// matches the UI, so adoption becomes a no-op.
static uint32_t s_acUserCmdMs   = 0;
static const uint32_t AC_SETTLE_MS = 12000;

// The A/C control state is seeded from the unit's telemetry exactly once (so the
// UI mirrors the unit at boot). Set true after seeding — and also the moment the
// user issues any A/C command, so a late seed (e.g. the unit was off at boot and
// only became readable later) can never clobber a value the user already chose.
static bool s_acSeeded = false;

// Reconcile the A/C control state with what the unit actually reports, so the UI
// mirrors reality at boot and whenever the unit is changed by its own remote or
// the official app. Skipped while a user command is pending or within the settle
// window (don't fight a change in flight) and only applied when the reported
// state differs from the current one (no needless LVGL churn every poll). The
// config fields (BatteryType/Power) are handled separately on Peripherals.
static void adoptAcStateFromUnit() {
    if (!openairCfgIsActive() || openairCmdPending()) return;
    uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (s_acUserCmdMs && (nowMs - s_acUserCmdMs) < AC_SETTLE_MS) return;
    OpenAirData d = openairGetData();
    if (!d.valid) return;

    static uint32_t adoptedFrame = 0;
    if (d.lastMs == adoptedFrame) return;   // this telemetry frame already handled
    adoptedFrame = d.lastMs;

    int   acMode   = (d.uPowerState == 0) ? 0 : (d.uMode == 1 ? 2 : 1);  // off/eco/cool
    bool  fanAuto  = (d.uMode == 0);
    int   fanSpeed = (d.uBlower >= 1 && d.uBlower <= 6) ? d.uBlower : 1;
    float setp     = d.uTempTenths / 10.0f;

    P4ControlState cur;
    p4GetControlState(cur);

    // Seed the FULL A/C control state once, from the first telemetry read, so the
    // UI mirrors the unit at boot. The setpoint is only trustworthy while the unit
    // is ON (an OFF unit reports 0), so seed it only then; power/mode/fan always.
    if (!s_acSeeded) {
        s_acSeeded = true;
        p4SetAcMode(acMode);
        p4SetAcFan(fanAuto, fanSpeed);
        p4SetAcFlaps(d.uFlaps1, d.uFlaps2);
        if (d.uPowerState != 0) p4SetRoomSetpoint(setp);
        s_acAdoptedFromUnit = true;
        return;
    }

    // Steady state: reconcile ONLY the power on/off state. The setpoint, cool/eco
    // sub-mode and fan level are user-owned, and the unit's telemetry for them
    // lags its real value by many seconds (the reported setpoint stays stale long
    // after a change). Continuously adopting them fought and reverted user input
    // (and echoed spurious commands back). On/off is discrete and worth mirroring
    // — e.g. the unit shutting itself off — but the SAME lag hits it too.
    bool unitCooling = (d.uPowerState != 0);
    bool uiCooling   = (cur.acMode != 0);

    // Confirmation gate: the unit's telemetry lags its real state by several
    // seconds after any change, including the post-write echo of our own command
    // (turn cool ON → the next few frames may still report OFF). A single frame
    // that disagrees with the UI is therefore NOT trusted: require the same on/off
    // disagreement on two consecutive distinct telemetry frames before adopting
    // it. A genuine remote/app change persists and confirms on the next poll; a
    // stale in-flight frame is superseded before it can revert the user's action.
    static int s_offReconcile = -1;   // -1 = idle; else the candidate unitCooling
    if (unitCooling == uiCooling) { s_offReconcile = -1; return; }
    if (s_offReconcile != (int)unitCooling) { s_offReconcile = (int)unitCooling; return; }
    s_offReconcile = -1;

    p4SetAcMode(unitCooling ? acMode : 0);
    if (unitCooling) p4SetAcFan(fanAuto, fanSpeed);
    s_acAdoptedFromUnit = true;
}

// Broadcast LCD-originated control changes every 100 ms.  Diffs the current
// control state against the previous snapshot; emits a WS `setting` frame for
// each field that changed so every connected browser tab updates without polling.
static void broadcastControlChanges(const P4ControlState& cs) {
    static P4ControlState prev = {};
    static bool          inited = false;
    char buf[WS_FRAME_BUF];

    auto emit = [&](const char* id, const char* value) {
        wsFmtSetting(buf, sizeof(buf), id, value);
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
    bool acChanged = false;
    if (!inited || cs.acMode != prev.acMode) {
        char v[8]; snprintf(v, sizeof(v), "%d", cs.acMode);
        emit("ac_mode", v);
        acChanged = true;
    }
    if (!inited || cs.acFanAuto != prev.acFanAuto) {
        emit("ac_fan_auto", cs.acFanAuto ? "1" : "0");
        acChanged = true;
    }
    if (!inited || cs.acFanSpeed != prev.acFanSpeed) {
        char v[8]; snprintf(v, sizeof(v), "%d", cs.acFanSpeed);
        emit("ac_fan_speed", v);
        acChanged = true;
    }
    // Flaps: track the genuine (post-init) change so the command below can mark
    // just that louver dirty — untouched flaps are echoed from the unit's shadow.
    bool flap1Changed = inited && cs.acFlap1 != prev.acFlap1;
    bool flap2Changed = inited && cs.acFlap2 != prev.acFlap2;
    if (!inited || cs.acFlap1 != prev.acFlap1) {
        emit("ac_flap1", cs.acFlap1 ? "1" : "0");
        acChanged = true;
    }
    if (!inited || cs.acFlap2 != prev.acFlap2) {
        emit("ac_flap2", cs.acFlap2 ? "1" : "0");
        acChanged = true;
    }
    // The setpoint stepper is SHARED between Truma heat and A/C cooling, so a
    // change to it only concerns the A/C while a cooling mode is actually
    // selected. Without this gate, nudging the room target in CALEFACCIÓN (or
    // with the A/C off) queues a BLE command to a unit that should be left
    // alone — which wakes it and makes it beep.
    bool acCooling = openairCfgIsActive() && !cs.heatingOn &&
                     (cs.acMode == 1 || cs.acMode == 2);
    if (acCooling && cs.roomSetpoint != prev.roomSetpoint) acChanged = true;

    // Push a setpoint to the unit ONLY when the user actually changed an A/C
    // control — never on init (the default control state is not the unit's real
    // state), never on an unchanged broadcast, and never when the change came
    // from the unit itself (adopted telemetry — echoing it back would re-apply
    // the command and beep). The unit re-applies any command it receives, so
    // writing every poll is both wrong and annoying; telemetry is read passively.
    bool adopted = s_acAdoptedFromUnit;
    s_acAdoptedFromUnit = false;
    if (acChanged && inited && !adopted) {
        OpenAirCmd cmd = buildOpenAirCmd(cs);
        // Only a louver the user actually moved is written; the rest stay echoed.
        if (flap1Changed) cmd.configDirty |= OA_CFG_FLAP1;
        if (flap2Changed) cmd.configDirty |= OA_CFG_FLAP2;
        openairSetCmd(cmd);
        // Open the settle window so adoption won't revert this change while the
        // command is still travelling to the unit (connect→write→readback).
        s_acUserCmdMs = (uint32_t)(esp_timer_get_time() / 1000ULL);
        // The user has taken control: a not-yet-run boot seed must not later
        // overwrite this value with stale telemetry.
        s_acSeeded = true;
    }

    prev   = cs;
    inited = true;
}

// Broadcast SSID/IP changes to connected browsers (footer shows "ssid / ip").
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

// Broadcast Victron + Ultimatron BLE data on change.
static void broadcastBleData() {
    static VictronData    prevV = {};
    static UltimatronData prevU = {};
    static bool           inited = false;
    char buf[WS_FRAME_BUF];

    VictronData v = victronGetData();
    if (!inited || victronChanged(v, prevV)) {
        wsFmtSolar(buf, sizeof(buf), v);
        wsQueueSend(buf);
        prevV = v;
    }

    UltimatronData u = ultimatronGetData();
    if (!inited || ultimatronChanged(u, prevU)) {
        wsFmtBatt(buf, sizeof(buf), u);
        wsQueueSend(buf);
        prevU = u;
    }
    inited = true;
}

// Broadcast Multiplus VE.Bus snapshot on change.
static void broadcastMultiplusData() {
    static MultiplusData prev   = {};
    static bool          inited = false;
    char buf[WS_FRAME_BUF];

    MultiplusData m = multiplusGetData();
    if (inited && !multiplusChanged(m, prev)) return;
    wsFmtMulti(buf, sizeof(buf), m);
    wsQueueSend(buf);
    prev   = m;
    inited = true;
}

// Broadcast OpenAir PLUS A/C telemetry on change.
static void broadcastOpenAirData() {
    static OpenAirData prev        = {};
    static bool        inited      = false;
    static bool        prevNeedPair = false;
    static bool        prevConn     = false;
    if (!openairIsConfigured()) return;

    OpenAirData d = openairGetData();
    bool needPair = openairNeedsPair();
    bool conn     = openairConnected();   // live telemetry, not a stale frame
    bool changed = !inited
                || d.valid       != prev.valid
                || d.errors      != prev.errors
                || d.blowerSpeedPct != prev.blowerSpeedPct
                || d.compressorSpeedRpm != prev.compressorSpeedRpm
                || needPair      != prevNeedPair
                || conn          != prevConn
                || fabsf(d.probe1C - prev.probe1C) > 0.5f
                || fabsf(d.probe2C - prev.probe2C) > 0.5f;
    if (!changed) return;

    char buf[WS_FRAME_BUF];
    wsFmtAc(buf, sizeof(buf), d, conn, needPair);
    wsQueueSend(buf);
    prev         = d;
    prevNeedPair = needPair;
    prevConn     = conn;
    inited       = true;
}

// Broadcast tank-level (BTHome) on change.
static void broadcastTankData() {
    static TankData prev   = {};
    static bool     inited = false;
    char buf[WS_FRAME_BUF];

    TankData t = tankGetData();
    if (!inited || tankChanged(t, prev)) {
        wsFmtTank(buf, sizeof(buf), t);
        wsQueueSend(buf);
        prev   = t;
        inited = true;
    }
}

// Broadcast LIN-derived values and AM2301 outdoor temperature on change.
static void broadcastLinTemps() {
    static TrumaLinSnapshot prev = {};
    static bool             inited = false;
    char buf[WS_FRAME_BUF];

    TrumaLinSnapshot lin;
    // No fresh read means nothing changed to broadcast — skip the tick rather
    // than diff against a fabricated snapshot.
    if (!trumaLinGetSnapshot(lin)) return;

    auto emit = [&](const char* id, const char* value) {
        wsFmtStatus(buf, sizeof(buf), id, value);
        wsQueueSend(buf);
    };

    char val[16];

    if (!inited || linTempChanged(lin.roomTemp, prev.roomTemp)) {
        wsFmtTemp(val, sizeof(val), lin.roomTemp);
        emit("room_temp", val);
    }
    if (!inited || linTempChanged(lin.waterTemp, prev.waterTemp)) {
        wsFmtTemp(val, sizeof(val), lin.waterTemp);
        emit("water_temp", val);
    }
    if (!inited || lin.waterHeating != prev.waterHeating) {
        emit("water_heating", lin.waterHeating ? "1" : "0");
    }
    if (!inited || lin.linOk != prev.linOk) {
        emit("linok", lin.linOk ? "1" : "0");
    }

    // Truma fault, gated on a class the Truma actually defines (trumaClassKnown)
    // so a misaligned 0x3D read can't fabricate one. The snapshot sends the same
    // pair on connect — keep both sides in step.
    {
        bool    fault = trumaClassKnown(lin.errClass);
        uint8_t cls   = fault ? lin.errClass : 0;
        uint8_t code  = fault ? lin.errCode  : 0;
        bool    pFault = trumaClassKnown(prev.errClass);
        if (!inited || cls != (pFault ? prev.errClass : 0)) {
            snprintf(val, sizeof(val), "%u", cls);
            emit("err_class", val);
        }
        if (!inited || code != (pFault ? prev.errCode : 0)) {
            snprintf(val, sizeof(val), "%u", code);
            emit("err_code", val);
        }
    }

    // AM2301 external sensor — not LIN-derived, but shares this slot to reuse
    // the temperature formatting and the change-detection cadence.
    {
        static float prevOutdoor = NAN;
        Am2301Data   am = am2301GetData();
        float        outdoor = am.valid ? am.tempC : NAN;
        if (!inited || linTempChanged(outdoor, prevOutdoor)) {
            wsFmtTemp(val, sizeof(val), outdoor);
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

    int ble = bleIconState();
    int tun = static_cast<int>(wstunnelUiState());

    char buf[WS_FRAME_BUF];
    if (ble != prevBle) {
        wsFmtIcon(buf, sizeof(buf), "ble", ble);
        wsQueueSend(buf);
        prevBle = ble;
    }
    if (tun != prevTun) {
        wsFmtIcon(buf, sizeof(buf), "tunnel", tun);
        wsQueueSend(buf);
        prevTun = tun;
    }
}

// Live-push OTA status when any user-visible field changes.  Diffing is
// mandatory: otherwise we'd queue an ota frame on every 100 ms tick.
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
        wsSendOtaFrame();
        prev = ota;
        inited = true;
    }
}

void wsPumpTask(void* /*arg*/) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        // Order matters: broadcast first so a user A/C change is turned into a
        // pending command (and opens the settle window) BEFORE adoption runs —
        // otherwise adoption would revert the change to stale telemetry.
        P4ControlState cs;
        p4GetControlState(cs);
        broadcastControlChanges(cs);
        adoptAcStateFromUnit();
        broadcastNetInfoChange();
        broadcastBleData();
        broadcastTankData();
        broadcastMultiplusData();
        broadcastOpenAirData();
        broadcastLinTemps();
        broadcastIconStates();
        broadcastOtaStatus();
        wsQueueDrain();
        p4OtaBeat(P4OTA_BEAT_WEB);   // liveness for the post-OTA self-test
    }
}
