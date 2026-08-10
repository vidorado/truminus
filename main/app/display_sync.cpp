#include "display_sync.hpp"
#include "boot_sequence.hpp"
#include "openair_config.hpp"
#include "victronble.hpp"
#include "ultimatronble.hpp"
#include "tankble.hpp"
#include "multiplusble.hpp"
#include "openairble.hpp"
#include "ble_status.hpp"
#include "truma_lin.hpp"
#include "lin_codec.hpp"
#include "am2301.hpp"
#include "wifi_manager.hpp"
#include "wstunnel.hpp"
#include "p4_ota.hpp"
#include "heapdiag.hpp"
#include "i18n.hpp"
#include <cmath>

void displaySyncInit(P4DisplayData& d) {
    d = {};
    d.roomTemp     = NAN;
    d.waterTemp    = NAN;
    d.outdoorTemp  = NAN;
    d.roomSetpoint = 20.0f;
}

void displaySyncTick(P4DisplayData& d, uint32_t iter) {
    p4OtaBeat(P4OTA_BEAT_LOOP);   // liveness for the post-OTA self-test

    // During a firmware install the full-screen OTA progress screen owns the
    // panel and refreshes itself (p4DisplaySetOtaProgress), so skip the whole-UI
    // rebuild below — the main screen is inactive under the OTA screen and
    // redrawing it every tick is wasted CPU/LVGL work. NOTE: this does NOT affect
    // the download's black↔cyan flicker; that is an L2-cache-off framebuffer
    // underrun below LVGL (see pio-idf-p4 skill), unfixable on this board and
    // accepted as cosmetic. This early-out is purely a CPU-saving cleanup.
    // Dismiss any stale dialog once as the install starts so it can't outlive the
    // switch to the OTA screen.
    static bool s_wasInstalling = false;
    bool installing = p4OtaInstalling();
    if (installing) {
        if (!s_wasInstalling) p4DisplayDismissDialogs();
        s_wasInstalling = true;
        return;
    }
    s_wasInstalling = false;

    // Steady-state internal-DRAM probe every 30 s — the `min` field is the
    // worst-case ever seen, i.e. how close we run to the OTA heap floor.
    if (iter % 30 == 0) heapDiagMark("steady");

    // Topbar status-icon "attempt started" flags (see p4display).  The latch
    // for BLE lives in bootBleAttempting() — it detects the first scan window
    // (~15 s after boot) so the FAILED grace clock doesn't start before scanning.
    d.wifiAttempting = bootWifiAttempting();
    d.linAttempting  = bootLinAttempting();
    d.bleAttempting  = bootBleAttempting();

    // WiFi status
    WifiStatus ws = wifi_manager_get_status();
    d.wifiOk = ws.connected;
    d.ssid   = ws.connected ? ws.ssid : nullptr;
    d.ip     = ws.connected ? ws.ip   : nullptr;

    // BLE / solar data
    VictronData    vd = victronGetData();
    UltimatronData ud = ultimatronGetData();

    if (vd.valid) {
        d.solar.valid   = true;
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
        d.batt.valid     = true;
        d.batt.flowValid = ud.flowValid;
        d.batt.soc       = (int)ud.soc;
        d.batt.voltageV  = ud.battV;
        d.batt.currentA  = ud.battA;
    } else {
        d.batt.valid     = false;
        d.batt.flowValid = false;
    }

    // Fresh-water tank — BTHome sensor.
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

    // LIN snapshot → room/water temp + LIN-ok dot.  Static so a failed lock take
    // carries the previous tick's values forward instead of blanking the panel;
    // this runs only on the main loop, so there is no sharing to guard.
    static TrumaLinSnapshot lin = { false, NAN, NAN, false, 0, 0, 0, 0 };
    trumaLinGetSnapshot(lin);
    d.linOk        = lin.linOk;
    d.roomTemp     = lin.roomTemp;
    d.waterTemp    = lin.waterTemp;
    d.waterHeating = lin.waterHeating;

    // AM2301 external sensor → outdoor temp (NAN until first valid read).
    Am2301Data am = am2301GetData();
    d.outdoorTemp = am.valid ? am.tempC : NAN;

    // Status line: shows the active alert(s) in red — "No LIN bus" and/or an
    // OpenAir A/C fault title.  When more than one is active they share the
    // single line by rotating one every 5 s (the status bar is one line, so
    // a No-LIN state must not permanently hide an A/C fault, or vice-versa).
    // Only pushes on a set change or a rotation tick so the slot stays free
    // for ad-hoc messages the rest of the time.
    {
        OpenAirData oas = openairGetData();
        // "A/C disconnected" feedback: surface it whenever the CLIMATIZACIÓN
        // panel is up and we're not heating (the A/C is out of play during Truma
        // heat), regardless of the selected mode. Gating it on cool/eco would
        // never fire — those buttons are disabled while disconnected, so the user
        // can't get into that state — leaving the greyed buttons unexplained.
        P4ControlState acs;
        p4GetControlState(acs);
        bool acDown = openairCfgIsActive() && !acs.heatingOn && !openairConnected();
        // Rejecting-handshake ("Bt") gets the actionable re-pair text; a plain
        // unreachable unit gets the generic offline line. Both outrank a stale
        // telemetry error (the cached frame is old).
        bool acRepair  = acDown && openairNeedsPair();
        bool acOffline = acDown && !openairNeedsPair();
        const char* acErr = (openairCfgIsActive() && !acDown && oas.valid)
                            ? openairErrorTitle(oas.errors) : nullptr;
        bool noLin = (iter >= 10 && !lin.linOk);

        // Prefix every alert with the FontAwesome warning triangle
        // (U+F071 "\xEF\x81\xB1", added to font_icons.ttf).  The status font
        // (s_font_20) falls back to the icon font, so the glyph renders even
        // though the text font has no warning sign of its own.
        static char buf0[80], buf1[80];
        const char* msgs[2];
        int nmsg = 0;
        if (noLin)      { snprintf(buf0, sizeof(buf0), "\xEF\x81\xB1 %s", t(TK::STATUS_NO_LIN)); msgs[nmsg++] = buf0; }
        if (acRepair)   { snprintf(buf1, sizeof(buf1), "\xEF\x81\xB1 %s", t(TK::AC_REPAIR));     msgs[nmsg++] = buf1; }
        else if (acOffline) { snprintf(buf1, sizeof(buf1), "\xEF\x81\xB1 %s", t(TK::AC_OFFLINE)); msgs[nmsg++] = buf1; }
        else if (acErr) { snprintf(buf1, sizeof(buf1), "\xEF\x81\xB1 %s", acErr);                msgs[nmsg++] = buf1; }

        // Signature of the active set: re-render immediately when it changes
        // (a new fault, a cleared fault, LIN up/down) so the user isn't left
        // looking at a stale line for up to 5 s.
        int sig = (noLin ? 1 : 0) | (acRepair ? 0x10000 : 0) | (acOffline ? 0x20000 : 0)
                | (acErr ? (oas.errors << 1) : 0);

        static int      prevSig    = -1;
        static int      cycleIdx   = 0;
        static uint32_t lastRotIt  = 0;
        static bool     inited     = false;

        bool setChanged = (sig != prevSig);
        if (setChanged) { cycleIdx = 0; prevSig = sig; }

        if (nmsg == 0) {
            // Clear the slot once when the last alert goes away (but never
            // wipe the very first boot "Iniciando…" before any alert showed).
            if (setChanged && inited) p4DisplaySetStatus("", false);
        } else {
            bool rotate = setChanged || (nmsg > 1 && (iter - lastRotIt) >= 5);
            if (rotate) {
                if (cycleIdx >= nmsg) cycleIdx = 0;
                p4DisplaySetStatus(msgs[cycleIdx], true);
                cycleIdx  = (cycleIdx + 1) % nmsg;
                lastRotIt = iter;
                inited    = true;
            }
        }
    }

    // Error modal (mirrors the web).  Dismissal is tracked INDEPENDENTLY per
    // peripheral: acknowledging a Truma fault doesn't clear an A/C one and
    // vice-versa.  A Truma fault takes priority for display (it still runs
    // heating + hot water even with the A/C configured), but each source
    // keeps its own ack so a masked fault re-surfaces only if not yet acked.
    {
        // A GetErrorInfo reply is only a real fault when its class is one the
        // Truma actually defines — trumaClassKnown() (lin_codec) owns that set
        // and is the shared gate for every fault surface, LCD and web alike.
        //
        // The CLASS decides whether there is a fault; the code only names it.
        // Keying on the code would swallow any fault the Truma reports with
        // code 0 — the appliance would sit locked out with a silent UI. Both
        // go into the identity so a new code under the same class re-pops the
        // modal instead of inheriting the previous dismissal (every known class
        // is non-zero, so the identity is non-zero exactly when there's a fault).
        int truma = trumaClassKnown(lin.errClass)
                    ? ((lin.errClass << 8) | lin.errCode) : 0;
        OpenAirData omd = openairGetData();
        int ac = (openairCfgIsActive() && omd.valid) ? omd.errors : 0;
        if (ac != 0 && !openairErrorTitle(ac)) ac = 0;   // unknown A/C code → ignore

        static int trumaAcked = -1, acAcked = -1;   // last dismissed code per source
        static int shownSrc   = 0,  shownCode = 0;   // fault the modal shows now

        // Attribute a dismiss tap to the fault currently on screen.
        if (p4DisplayErrorModalDismissed()) {
            if (shownSrc == 1)      trumaAcked = shownCode;
            else if (shownSrc == 2) acAcked    = shownCode;
            shownSrc = 0; shownCode = 0;
        }
        // Forget a dismissal once its source clears, so the same code can
        // re-pop later (matches the web: ack resets when the code changes).
        if (truma == 0) trumaAcked = -1;
        if (ac    == 0) acAcked    = -1;

        int  actSrc  = truma ? 1 : (ac ? 2 : 0);     // Truma priority
        int  actCode = truma ? truma : ac;
        bool acked   = (actSrc == 1 && actCode == trumaAcked) ||
                       (actSrc == 2 && actCode == acAcked);
        bool show    = (actSrc != 0) && !acked;

        if (!show) {
            if (shownSrc) { p4DisplayHideErrorModal(); shownSrc = 0; shownCode = 0; }
        } else if (actSrc != shownSrc || actCode != shownCode) {
            const char* mTitle; const char* mSub = nullptr; const char* mDesc;
            uint32_t    mColor;
            static char subBuf[48];
            if (actSrc == 1) {
                TK sevTk;
                if (lin.errClass == 1 || lin.errClass == 2) { sevTk = TK::WARN_LBL;   mColor = 0xffaa00; }
                else if (lin.errClass == 40)                { sevTk = TK::LOCKED_LBL; mColor = 0xcc2222; }
                else                                        { sevTk = TK::ERROR_LBL;  mColor = 0xff4444; }
                snprintf(subBuf, sizeof(subBuf), t(TK::ERR_SUBTITLE_FMT),
                         lin.errClass, lin.errCode);
                mTitle = t(sevTk); mSub = subBuf; mDesc = "";
            } else {
                mTitle = openairErrorTitle(actCode);   // e.g. "A/C: Error de inclinación"
                mDesc  = openairErrorDesc(actCode);
                mColor = 0xff4444;                     // all A/C faults are red
            }
            if (p4DisplayShowErrorModal(mTitle, mSub, mDesc, mColor)) {
                shownSrc = actSrc; shownCode = actCode;
            }
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
    OpenAirData oad = openairGetData();
    d.bleState = bleIconState();

    d.openairConfigured = openairCfgIsActive();
    // "Connected" = live telemetry, not merely a cached frame — so the snowflake
    // strikes through the moment the unit drops off, even though oad.valid stays
    // true holding the last frame.
    d.acConnected       = openairConnected();
    d.acCompressorOn    = d.acConnected && oad.compressorSpeedRpm > 0;

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
