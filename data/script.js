'use strict';

// ── WebSocket ─────────────────────────────────────────────────────────────
// Match the page's scheme: when the page is served over HTTPS (typical
// through the Plesk reverse tunnel) browsers reject `ws://` as mixed
// content and refuse to open it.
var wsScheme = (window.location.protocol === 'https:') ? 'wss://' : 'ws://';
var gateway  = wsScheme + window.location.hostname +
               (window.location.port ? ':' + window.location.port : '') + '/ws';
var wserror      = true;
var linerror     = false;

// ── Application state ─────────────────────────────────────────────────────
var s_temp        = 20.0;
var s_roomTemp    = null;
var s_heat        = false;
var s_boiler      = 'off';
var s_fan         = 'off';
var s_fanLevel    = 5;
var s_waterDemand = false;
var s_waterTemp   = null;
var s_errClass    = 0;
var s_errCode     = 0;
var s_errAcked    = false;

// ── Status bar ────────────────────────────────────────────────────────────
function updateStatusBar() {
    var msg = document.getElementById('statusMsg');
    var ip  = document.getElementById('deviceIp');
    if (!msg) return;
    // Always show the host the browser actually reached the page through:
    // the public domain via the WSS tunnel, or the LAN IP when accessed
    // locally.  The device's SSID / LAN IP aren't meaningful to a remote
    // client, so we don't surface them here.
    var ipText = window.location.hostname;
    if (wserror) {
        msg.textContent  = t('ws_conn');
        msg.style.color  = '#ffaa00';
        if (ip) { ip.textContent = ''; }
    } else if (linerror) {
        msg.textContent  = t('ws_no_lin');
        msg.style.color  = '#ff4444';
        if (ip) { ip.textContent = ipText; }
    } else {
        msg.textContent  = '';
        msg.style.color  = '';
        if (ip) { ip.textContent = ipText; }
    }
}
updateStatusBar();
// Hide mobile browser address bar by scrolling past the URL chrome.
setTimeout(function () { window.scrollTo(0, 1); }, 0);
// Apply the default language (es) to every data-i18n element immediately so
// the HTML fallback ("Off", etc.) is never visible before the snapshot
// arrives.  When the snapshot brings d.lang, applySetting('lang', ...) will
// re-apply if it differs.
applyLanguage(s_lang);

// ── WebSocket ─────────────────────────────────────────────────────────────
var ws = new ReconnectingWebSocket(gateway);
// Liveness is handled by WS control PING (opcode 0x9) sent every 20 s from
// the firmware; browsers auto-respond with PONG.  No JS heartbeat needed.

// Debounce the "WS down" indicator: brief reconnect cycles (under 3 s)
// would otherwise flicker the WiFi dot and the "Connecting..." footer.
// The physical CYD display never shows transient drops, mirror that UX.
var s_wsDownTimer = null;
ws.onopen = function () {
    // The device just rebooted after an OTA — reload to pick up new web assets.
    if (s_otaReloadPending) { location.reload(); return; }
    if (s_wsDownTimer) { clearTimeout(s_wsDownTimer); s_wsDownTimer = null; }
    wserror = false;
    updateStatusBar();
    setDot('dot-wifi', 'ok');
    ws.send('settings');
};
ws.onclose = function () {
    // If the socket drops mid-install, the device is rebooting into the new
    // image — arm a one-shot page reload for when the socket comes back.
    if (s_otaInstalling) s_otaReloadPending = true;
    if (s_wsDownTimer) return;   // already counting down
    s_wsDownTimer = setTimeout(function () {
        s_wsDownTimer = null;
        wserror = true;
        updateStatusBar();
        setDot('dot-wifi', 'err');
        setDot('dot-lin',  'err');
    }, 3000);
};
// Re-request the full snapshot whenever the tab returns to the foreground.
// A phone that slept may have missed broadcasts (or hold a half-open socket
// that hasn't reconnected yet); this refreshes the state without waiting for
// TCP to notice the dead peer.  When the socket is already gone, onopen will
// send 'settings' on reconnect anyway, so we only act on a live socket.
//
// If the tab was hidden for more than STALE_AFTER_MS, the values on screen may
// be badly out of date.  Cover the UI with a spinner veil until the fresh
// snapshot lands so the user doesn't read stale data right before it flips to
// the correct values.  hideReloadVeil() runs when the 'snapshot' arrives, with
// a safety timeout in case it never does.
var STALE_AFTER_MS  = 60000;
var VEIL_TIMEOUT_MS = 12000;
var s_hiddenAt      = 0;
var s_veilTimer     = null;
function showReloadVeil() {
    var v  = document.getElementById('reload-veil');
    var sp = document.getElementById('reload-spinner-box');
    var er = document.getElementById('reload-error');
    if (v)  v.hidden  = false;
    if (sp) sp.hidden = false;   // spinning / reconnecting state
    if (er) er.hidden = true;
    if (s_veilTimer) clearTimeout(s_veilTimer);
    s_veilTimer = setTimeout(reloadVeilTimedOut, VEIL_TIMEOUT_MS);
}
// No snapshot arrived in time: keep the veil up but swap the spinner for an
// error message + Retry button so the user knows the data is still stale.
function reloadVeilTimedOut() {
    s_veilTimer = null;
    var sp = document.getElementById('reload-spinner-box');
    var er = document.getElementById('reload-error');
    if (sp) sp.hidden = true;
    if (er) er.hidden = false;
}
function retryReload() {
    showReloadVeil();
    ws.refresh();   // drop the (possibly half-dead) socket and reconnect
}
function hideReloadVeil() {
    var v = document.getElementById('reload-veil');
    if (v) v.hidden = true;
    if (s_veilTimer) { clearTimeout(s_veilTimer); s_veilTimer = null; }
}
document.addEventListener('visibilitychange', function () {
    if (document.hidden) { s_hiddenAt = Date.now(); return; }
    var stale = s_hiddenAt && (Date.now() - s_hiddenAt) > STALE_AFTER_MS;
    s_hiddenAt = 0;
    if (!stale) {
        // Short absence: a live socket can just re-pull the snapshot.
        if (ws.readyState === 1) ws.send('settings');
        return;
    }
    // Long sleep: the socket often looks OPEN but is half-dead (very common
    // through the WSS tunnel — writes vanish into a dead pipe and no snapshot
    // ever comes back).  Don't trust readyState; force a fresh reconnect and
    // let onopen re-send 'settings'.  The snapshot then hides the veil.
    showReloadVeil();
    ws.refresh();
});
// Escape closes the About overlay.
document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape') closeAbout();
});
ws.onmessage = function (event) {
    var d = JSON.parse(event.data);
    if (!d.command) return;
    if (d.command === 'solar') { applySolar(d); return; }
    if (d.command === 'batt')  { applyBatt(d);  return; }
    if (d.command === 'tank')  { applyTank(d);  return; }
    if (d.command === 'multi') { applyMulti(d); return; }
    if (d.command === 'icon')  { applyIcon(d);  return; }
    if (d.command === 'ota')   { applyOta(d);   return; }
    if (d.command === 'diag')  { s_diag = d; renderFault(); return; }
    if (d.command === 'snapshot') {
        // Full initial-state burst from wsConnected() on the server.
        // One message with every cached setting and status value to
        // avoid flooding the per-client WS queue with 30+ messages.
        if (d.settings) {
            for (var k in d.settings) {
                applySetting(k.replace(/^\//, ''), d.settings[k]);
            }
        }
        if (d.status) {
            for (var k2 in d.status) {
                applyStatus(k2.replace(/^\//, ''), d.status[k2]);
            }
        }
        if (d.ssid !== undefined)         applyStatus('ssid', d.ssid);
        if (d.ip   !== undefined)         applyStatus('ip',   d.ip);
        if (d.outdoor_temp !== undefined) applyStatus('outdoor_temp', d.outdoor_temp);
        if (d.energy_idx !== undefined)   applySetting('energy_idx', d.energy_idx);
        if (d.lang !== undefined)         applySetting('lang', d.lang);
        hideReloadVeil();   // fresh state is in — drop the re-focus veil
        return;
    }
    if (!d.id) return;
    var id = d.id.replace(/^\//, '');
    if (d.command === 'setting') applySetting(id, d.value);
    else                         applyStatus(id, d.value);
};

// ── Send to device ────────────────────────────────────────────────────────
function send(id, value) {
    if (ws.readyState === 1)
        ws.send(JSON.stringify({ id: id, value: String(value) }));
}

// ── Status dot ────────────────────────────────────────────────────────────
function setDot(id, state) {
    var el = document.getElementById(id);
    if (el) el.className = 'sdot sdot-' + state;
}

// ── Icon state (BLE / tunnel) ────────────────────────────────────────────
// state: 0=disabled(grey), 1=configured-no-data(amber), 2=connected(blue)
// tunnel adds: 1=connecting(blink), 3=failed(red)
var BLE_DOT    = { 0: 'dis', 1: 'warn', 2: 'ok' };
var TUNNEL_DOT = { 0: 'dis', 1: 'warn', 2: 'ok', 3: 'err' };
function applyIcon(d) {
    if (d.id === 'ble') {
        setDot('dot-bt', BLE_DOT[d.state] || 'dis');
    } else if (d.id === 'tunnel') {
        setDot('dot-cloud', TUNNEL_DOT[d.state] || 'dis');
    }
}

// ── OTA update banner ─────────────────────────────────────────────────────
// Driven by {"command":"ota",available,installing,progress,current,latest,error}
// broadcast from main/p4_ota.cpp.  Shown only when an update is available or
// an install is in progress; the button triggers /ota_install on the device.
var s_otaInstalling = false;
var s_ota = null;            // last OTA frame, for the About overlay
var s_otaDismissedVer = null; // "Later"-dismissed version (banner stays hidden for it)
var s_otaReloadPending = false; // reload the page once the device reboots after an install
var s_diag = null;           // last {command:diag} frame (uncontrolled-fault record)
function applyOta(d) {
    s_ota = d;
    updateAbout(d);
    var banner = document.getElementById('ota-banner');
    var msg    = document.getElementById('ota-msg');
    var btn    = document.getElementById('ota-btn');
    var later  = document.getElementById('ota-later');
    if (!banner || !msg || !btn) return;

    s_otaInstalling = !!d.installing;

    // While the About overlay is open it already shows the OTA status, so the
    // banner would be redundant — keep it hidden until the overlay closes.
    var about = document.getElementById('about-modal');
    if (about && !about.classList.contains('hidden')) { banner.hidden = true; return; }

    var cancel = document.getElementById('ota-cancel');

    if (d.installing) {
        var pct = (typeof d.progress === 'number') ? d.progress : 0;
        // The Update button itself becomes the progress bar (near-white fill).
        btn.hidden = false;
        btn.disabled = true;
        btn.textContent = (pct >= 100) ? t('ota_reboot')
                                       : t('ota_updating') + ' ' + pct + '%';
        setBtnProgress(btn, pct);
        msg.textContent = (d.current || '?') + ' → ' + (d.latest || '?');
        if (later)  later.hidden  = true;
        if (cancel) cancel.hidden = (pct >= 100);   // no cancel once flashing is done
        banner.classList.remove('ota-err');
        banner.hidden = false;
        return;
    }

    clearBtnProgress(btn);
    if (cancel) cancel.hidden = true;
    btn.hidden = false;
    btn.disabled = false;
    btn.textContent = t('ota_update');
    if (later) later.hidden = false;

    if (d.error) {
        msg.textContent = t('ota_failed') + ': ' + d.error;
        banner.classList.add('ota-err');
        banner.hidden = false;
        return;
    }

    banner.classList.remove('ota-err');
    if (d.available && d.latest !== s_otaDismissedVer) {
        msg.textContent = '';
        var ttl = document.createElement('div');
        ttl.className = 'ota-title';
        ttl.textContent = t('ota_available');
        msg.appendChild(ttl);
        msg.appendChild(document.createTextNode(
            (d.current || '?') + ' → ' + (d.latest || '?')));
        banner.hidden = false;
    } else {
        banner.hidden = true;
    }
}

// "Later": dismiss the banner for this version (the About overlay still offers
// the manual install). A newer release re-shows it.
function otaDismiss() {
    if (s_ota) s_otaDismissedVer = s_ota.latest || null;
    var banner = document.getElementById('ota-banner');
    if (banner) banner.hidden = true;
}

function otaInstall() {
    if (s_otaInstalling) return;
    // Feedback on whichever button was used (banner or About overlay).
    ['ota-btn', 'about-install'].forEach(function (id) {
        var b = document.getElementById(id);
        if (b) { b.disabled = true; b.textContent = t('ota_updating'); }
    });
    // "Later" makes no sense once the update is running — hide it immediately
    // (don't wait for the first installing frame).
    var later = document.getElementById('ota-later');
    if (later) later.hidden = true;
    send('/ota_install', '1');
}

function otaCancel() {
    ['ota-cancel', 'about-cancel'].forEach(function (id) {
        var b = document.getElementById(id);
        if (b) b.disabled = true;
    });
    send('/ota_cancel', '1');
}

// Paint a button as a left-to-right progress bar with a near-white fill.
function setBtnProgress(btn, pct) {
    if (!btn) return;
    var p = Math.max(0, Math.min(100, pct));
    // Fill = white, track = a light blue-grey that's distinct from the blue
    // banner (else the unfilled part blends in and the bar looks invisible).
    // Dark text stays readable on both halves.
    btn.style.background = 'linear-gradient(90deg, #ffffff ' + p + '%, #aab6d4 ' + p + '%)';
    btn.style.color = '#16213e';
    btn.style.opacity = '1';   // counter the :disabled dim so the fill stays bright
}
function clearBtnProgress(btn) {
    if (!btn) return;
    btn.style.background = '';
    btn.style.color = '';
    btn.style.opacity = '';
}

// ── About / manual OTA overlay ────────────────────────────────────────────
// The footer logo opens this; it offers a manual "Check" and an "Install"
// button for any newer release (including patch X.Y.Z, which raise no banner
// nor LCD modal).  Live status arrives via the broadcast `ota` frame.
// Render the last uncontrolled-fault line in the About overlay (hidden when
// the device has no fault on record).
function renderFault() {
    var el = document.getElementById('about-fault');
    if (!el) return;
    if (s_diag && s_diag.fault) {
        el.textContent = t('last_fault') + ': ' + s_diag.fault +
                         ' (×' + (s_diag.count || 0) + ', fw ' + (s_diag.fw || '?') + ')';
        el.hidden = false;
    } else {
        el.hidden = true;
    }
}

function openAbout() {
    var modal = document.getElementById('about-modal');
    if (!modal) return;
    updateAbout(s_ota);
    renderFault();
    modal.classList.remove('hidden');
    // Hide the banner right away (don't wait for the next OTA push).
    var banner = document.getElementById('ota-banner');
    if (banner) banner.hidden = true;
}

function closeAbout() {
    var modal = document.getElementById('about-modal');
    if (modal) modal.classList.add('hidden');
    // Re-evaluate the banner now the overlay no longer covers the OTA status.
    if (s_ota) applyOta(s_ota);
}

function otaCheck() {
    var btn = document.getElementById('about-check');
    if (btn) { btn.disabled = true; btn.textContent = t('ota_checking'); }
    send('/ota_check', '1');
}

// Render the overlay from an OTA frame (or null before the first one).
function updateAbout(d) {
    var cur     = document.getElementById('about-current');
    var latest  = document.getElementById('about-latest');
    var prog    = document.getElementById('about-progress');
    var check   = document.getElementById('about-check');
    var install = document.getElementById('about-install');
    if (!cur || !latest || !prog || !check || !install) return;

    if (!d) { cur.textContent = '--'; return; }

    cur.textContent = d.current || '?';

    var cancel = document.getElementById('about-cancel');

    if (d.installing) {
        var pct = (typeof d.progress === 'number') ? d.progress : 0;
        // The Update button becomes the progress bar; Cancel sits beside it.
        install.hidden = false;
        install.disabled = true;
        install.textContent = (pct >= 100) ? t('ota_reboot')
                                           : t('ota_updating') + ' ' + pct + '%';
        setBtnProgress(install, pct);
        prog.hidden = true;
        latest.hidden = true;
        check.hidden = true;
        if (cancel) cancel.hidden = (pct >= 100);
        return;
    }

    clearBtnProgress(install);
    if (cancel) cancel.hidden = true;
    prog.hidden = true;
    check.hidden = false;

    // Authoritative "checking" state — fixes the button getting stuck on
    // "Checking…" when the result frame matched the previous one.
    if (d.checking) {
        check.disabled = true;
        check.textContent = t('ota_checking');
        latest.hidden = true;
        install.hidden = true;
        return;
    }

    check.disabled = false;
    check.textContent = t('ota_check');

    if (d.error) {
        latest.textContent = t('ota_failed') + ': ' + d.error;
        latest.hidden = false;
        install.hidden = true;
        return;
    }

    if (d.available) {
        latest.textContent = t('ota_available') + ': v' + (d.latest || '?');
        latest.hidden = false;
        install.hidden = false;
        install.disabled = false;
        install.textContent = t('ota_update');
    } else {
        latest.textContent = t('ota_uptodate');
        latest.hidden = false;
        install.hidden = true;
    }
}

// ── Settings received from device ─────────────────────────────────────────
function applySetting(id, value) {
    if (id === 'temp') {
        s_temp = parseFloat(value) || 20;
        refreshSetpoint();
        refreshIndicators();
    } else if (id === 'heating') {
        s_heat = (value === '1');
        refreshHeat();
    } else if (id === 'boiler') {
        s_boiler = value;
        refreshBoiler();
    } else if (id === 'fan') {
        s_fan = value;
        var n = parseInt(value);
        if (!isNaN(n) && n > 0) s_fanLevel = n;
        refreshFan();
    } else if (id === 'lang') {
        applyLanguage(value);
    }
}

// ── Status received from device ───────────────────────────────────────────
function applyStatus(id, value) {
    // For temperature topics, swap the Truma "-273" sentinel (and any
    // value below -200) for "--", matching the physical CYD behaviour.
    var displayValue = value;
    if (id === 'room_temp' || id === 'water_temp' || id === 'outdoor_temp') {
        var f = parseFloat(value);
        if (isNaN(f) || f <= -200) displayValue = '--';
    }
    var el = document.getElementById(id);
    if (el) el.textContent = displayValue;

    // ssid / ip are still reported by the device but no longer displayed on
    // the web status bar (we show the URL host instead); ignore them here.
    if (id === 'ssid' || id === 'ip') return;

    if (id === 'linok') {
        linerror = parseInt(value) !== 1;
        updateStatusBar();
        setDot('dot-lin', linerror ? 'err' : 'ok');
        // Indicators are LIN-gated — re-render so they dim on bus loss
        // and re-illuminate when the bus comes back.
        refreshIndicators();
    }

    if (id === 'water_heating') {
        s_waterDemand = parseInt(value) === 1;
        refreshIndicators();
    }

    if (id === 'water_temp') {
        s_waterTemp = parseFloat(value);
        refreshIndicators();
    }

    if (id === 'room_temp') {
        s_roomTemp = parseFloat(value);
        refreshIndicators();
    }

    if (id === 'err_class') {
        var newClass = parseInt(value) || 0;
        if (newClass !== s_errClass) { s_errClass = newClass; s_errAcked = false; }
        updateErrorDisplay();
    }

    if (id === 'err_code') {
        var newCode = parseInt(value) || 0;
        if (newCode !== s_errCode) {
            s_errCode = newCode;
            if (newCode !== 0) s_errAcked = false;
        }
        updateErrorDisplay();
    }
}

// ── UI refresh functions ──────────────────────────────────────────────────

function cls(id, cssClass, on) {
    var el = document.getElementById(id);
    if (el) el.classList.toggle(cssClass, on);
}

function refreshSetpoint() {
    document.getElementById('spVal').textContent = s_temp.toFixed(1) + ' °C';
}

function refreshHeat() {
    cls('hBtnOn',  'btn-sel',  s_heat);
    cls('hBtnOff', 'btn-sel', !s_heat);
    cls('spRow',      'sp-hidden', !s_heat);
    cls('fanHeatRow', 'vis-hidden', !s_heat);
    cls('fanSbyRow',  'vis-hidden',  s_heat);
    if (s_heat) cls('fanLvlRow', 'vis-hidden', true);
    refreshFan();
    refreshIndicators();
}

function refreshFan() {
    if (s_heat) {
        cls('fhEco',  'btn-sel', s_fan === 'eco');
        cls('fhHigh', 'btn-sel', s_fan === 'high');
        cls('fhOff',  'btn-sel', s_fan === 'off');
    } else {
        var fanOn = (s_fan !== 'off' && s_fan !== '0' && s_fan !== '');
        cls('fsBtnOn',  'btn-sel',  fanOn);
        cls('fsBtnOff', 'btn-sel', !fanOn);
        cls('fanLvlRow', 'vis-hidden', !fanOn);
        document.getElementById('fanLvlVal').textContent = s_fanLevel;
    }
}

function refreshBoiler() {
    var map = { off: 'bOff', eco: 'bEco', high: 'bHigh', boost: 'bBoost' };
    Object.keys(map).forEach(function (k) {
        cls(map[k], 'btn-sel', s_boiler === k);
    });
    refreshIndicators();
}

function refreshIndicators() {
    // Without a live LIN bus the boiler/heat fields are stale (settings
    // queued but not actually applied by the Truma), so force-dim the
    // indicators.  Mirrors the LCD gate in p4DisplayUpdate.
    var linOk    = !linerror;
    var boilerOn = linOk && s_boiler !== 'off';
    var boilerSetTemp = { eco: 40, high: 60, boost: 60 };
    var wSet = boilerSetTemp[s_boiler] || 0;
    var wAtTemp = boilerOn && s_waterTemp !== null && s_waterTemp > 0 && s_waterTemp >= wSet - 1;
    var wDemand = boilerOn && s_waterDemand && !wAtTemp;
    cls('ind-tint', 'ind-on',     boilerOn && !wDemand);
    cls('ind-tint', 'ind-active', wDemand);

    var heatOn      = linOk && s_heat;
    var heatDemand  = heatOn && s_roomTemp !== null && (s_roomTemp < s_temp - 0.3);
    cls('ind-fire', 'ind-on',     heatOn && !heatDemand);
    cls('ind-fire', 'ind-active', heatDemand);

    // Water temperature bar (matches CYD display)
    var wTempVal = document.getElementById('water_temp_val');
    var wTempFill = document.getElementById('water_temp_fill');
    if (wTempVal) {
        if (s_waterTemp !== null && s_waterTemp > -200) {
            wTempVal.textContent = s_waterTemp.toFixed(0);
        } else {
            wTempVal.textContent = '--';
        }
    }
    if (wTempFill) {
        // Scale tops out at the selected boiler target (40 °C in eco, 60 °C
        // otherwise) so the fill rescales with the setpoint, matching the LCD
        // boiler indicator (see main/p4display.cpp). Colour thresholds below
        // stay absolute.
        var WTEMP_SCALE = (s_boiler === 'eco') ? 40 : 60;
        var pct = 0;
        if (s_waterTemp !== null && s_waterTemp > 0) {
            pct = Math.min(100, Math.max(0, (s_waterTemp / WTEMP_SCALE) * 100));
        }
        wTempFill.style.height = pct + '%';

        // Colour thresholds: blue <30 °C, amber 30-51 °C, red ≥51 °C.
        // (Matches the original "high" boiler mode behaviour: 50 % / 85 %
        // of a 60 °C target.)
        var t = s_waterTemp || 0;
        var wCol = (t < 30) ? '#4488ff'
                 : (t < 51) ? '#ffbb00'
                            : '#ff3333';
        wTempFill.style.background = wCol;
        wTempFill.style.opacity = '1';
        // Boiler body is intentionally static when heating — only the
        // topbar drop icon (#ind-tint) blinks to signal demand, matching
        // the CYD physical layout.
        var body = document.querySelector('.water-temp-body');
        if (body) {
            body.style.borderColor = '#8888aa';
            body.style.animation = '';
        }
    }
}

// ── Debounced send (300 ms per topic) ─────────────────────────────────────
var _sendTimers = {};
function sendDebounced(id, value) {
    clearTimeout(_sendTimers[id]);
    _sendTimers[id] = setTimeout(function () { send(id, value); }, 300);
}

// ── User actions ──────────────────────────────────────────────────────────

function changeTemp(delta) {
    s_temp = Math.round((s_temp + delta) * 2) / 2;
    if (s_temp < 5)  s_temp = 5;
    if (s_temp > 30) s_temp = 30;
    refreshSetpoint();
    refreshIndicators();
    sendDebounced('/temp', s_temp.toFixed(1));
}

// All button actions use sendDebounced (300 ms) so rapid taps coalesce to
// the last state — matches the physical screen's touch behaviour.
function setHeating(on) {
    s_heat = on;
    if (!s_heat) { s_fan = 'off'; sendDebounced('/fan', 'off'); }
    sendDebounced('/heating', s_heat ? '1' : '0');
    refreshHeat();
}

function setFan(value) {
    s_fan = value;
    sendDebounced('/fan', value);
    refreshFan();
}

function setFanSby(mode) {
    s_fan = (mode === 'on') ? String(s_fanLevel) : 'off';
    sendDebounced('/fan', s_fan);
    refreshFan();
}

function changeFanLvl(delta) {
    s_fanLevel = Math.min(10, Math.max(1, s_fanLevel + delta));
    if (s_fan !== 'off') {
        s_fan = String(s_fanLevel);
        sendDebounced('/fan', s_fan);
    }
    document.getElementById('fanLvlVal').textContent = s_fanLevel;
}

function setBoiler(value) {
    s_boiler = value;
    sendDebounced('/boiler', value);
    refreshBoiler();
}

// ── Truma error display ───────────────────────────────────────────────────
function errSeverity(cls) {
    if (cls === 0)              return 'none';
    if (cls === 1 || cls === 2) return 'warn';
    if (cls === 40)             return 'locked';
    return 'error';
}

var ERR_COLOR = { none: '', warn: '#ffaa00', error: '#ff4444', locked: '#cc2222' };

function updateErrorDisplay() {
    var line = document.getElementById('error_line');
    if (!line) return;

    if (s_errCode === 0) {
        line.textContent = '';
        line.style.color = '';
        hideErrorModal();
        s_errAcked = false;
        return;
    }

    var sev  = errSeverity(s_errClass);
    var col  = ERR_COLOR[sev];
    var lbl  = t('err_' + sev) || sev.toUpperCase();
    var desc = (typeof ErrText !== 'undefined' && ErrText[s_errCode])
               ? ErrText[s_errCode] : 'Code ' + s_errCode;

    line.textContent = lbl + '  (class ' + s_errClass + ' / code ' + s_errCode + ')  ' + desc;
    line.style.color = col;

    if (!s_errAcked) showErrorModal(lbl, col, desc);
}

function showErrorModal(label, color, desc) {
    var modal = document.getElementById('err-modal');
    if (!modal) return;
    document.getElementById('err-modal-title').textContent = label;
    document.getElementById('err-modal-title').style.color = color;
    document.getElementById('err-modal-sub').textContent   =
        'Class ' + s_errClass + ' / Code ' + s_errCode;
    document.getElementById('err-modal-desc').textContent  = desc;
    modal.classList.remove('hidden');
}

function hideErrorModal() {
    var modal = document.getElementById('err-modal');
    if (modal) modal.classList.add('hidden');
}

function acknowledgeError() {
    s_errAcked = true;
    hideErrorModal();
}

// ── Solar charge / Battery BMS ────────────────────────────────────────────

// Format an integer watt value grouping thousands with a dot ("1.234"),
// preserving the sign. Used for every panel port reading.
function fmtW(n) {
    n = Math.round(Number(n) || 0);
    var s = Math.abs(n).toString().replace(/\B(?=(\d{3})+(?!\d))/g, '.');
    return (n < 0 ? '-' : '') + s;
}

var SOLAR_STATES = {
    0: 'sol_off', 2: 'sol_fault', 3: 'sol_bulk',
    4: 'sol_abs', 5: 'sol_float', 7: 'sol_equalize',
    245: 'sol_bulk', 247: 'sol_equalize', 252: 'sol_off'
};

function applySolar(d) {
    var stateLbl = document.getElementById('solar_state');
    if (!stateLbl) return;

    if (!d.valid) {
        stateLbl.textContent = '--';
        document.getElementById('solar_pvW').textContent  = '--';
        document.getElementById('solar_kWh').textContent  = '--';
        document.getElementById('solar_battA').textContent = '--';
        return;
    }
    stateLbl.textContent = t(SOLAR_STATES[d.state] || 'sol_off');
    document.getElementById('solar_pvW').textContent   = fmtW(d.pvW);
    document.getElementById('solar_kWh').textContent   = d.kWh;
    document.getElementById('solar_battA').textContent = d.battA;
}

function applyTank(d) {
    var lbl  = document.getElementById('tank_pct_val');
    var fill = document.getElementById('tank_fill');
    if (!d.valid) {
        if (lbl)  lbl.textContent = '--';
        if (fill) fill.style.height = '0%';
        return;
    }
    var pct = parseInt(d.pct);
    if (isNaN(pct)) pct = 0;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    if (lbl) lbl.textContent = pct;
    if (fill) {
        fill.style.height = pct + '%';
        // Same red→amber→blue ramp as the LCD's lv_bar (C_RED / C_AMBER_BAR
        // / C_WATER_COLD).  Empty stays the dark tank colour.
        var col = pct < 20 ? '#ff4444'
                : pct < 50 ? '#ffaa00'
                           : '#4488ff';
        fill.style.background = col;
    }
}

var MULTI_STATES = {
    0:   'inv_st_off',    1:   'inv_st_lowpower',  2:   'inv_st_fault',
    3:   'inv_st_bulk',   4:   'inv_st_abs',       5:   'inv_st_float',
    6:   'inv_st_storage',7:   'inv_st_equalize',  8:   'inv_st_passthru',
    9:   'inv_st_inv',   10:   'inv_st_assist',   11:   'inv_st_supply',
    252: 'inv_st_ext'
};

function applyMulti(d) {
    var st  = document.getElementById('inv_state');
    var rm  = document.getElementById('inv_mains_w');
    var rl  = document.getElementById('inv_load_w');
    var rb  = document.getElementById('inv_batt_w');
    var fm  = document.getElementById('flow_mains');
    var fl  = document.getElementById('flow_load');
    var fb  = document.getElementById('flow_batt');
    var mp  = document.getElementById('inv_mains_port');
    var lp  = document.getElementById('inv_load_port');
    var bp  = document.getElementById('inv_batt_port');

    if (!d.valid) {
        if (st) st.textContent = '--';
        if (rm) rm.textContent = '--';
        if (rl) rl.textContent = '--';
        if (rb) rb.textContent = '--';
        if (fm) { fm.classList.remove('active', 'flow-r', 'flow-l'); }
        if (fl) { fl.classList.remove('active', 'flow-r', 'flow-l'); }
        if (fb) { fb.classList.remove('active', 'flow-r', 'flow-l'); }
        if (mp) mp.classList.remove('ac-on');
        if (lp) lp.classList.remove('load-on');
        if (bp) { bp.classList.remove('batt-chg'); bp.classList.remove('batt-dis'); }
        return;
    }
    if (st) st.textContent = t(MULTI_STATES[d.state] || 'inv_st_off');
    // ac_in_w / ac_out_w arrive as null when the inverter is off / not
    // reporting (VE.Bus no-data sentinel) — show "--" and keep the flow idle.
    var inNa  = (d.ac_in_w  === null || d.ac_in_w  === undefined);
    var outNa = (d.ac_out_w === null || d.ac_out_w === undefined);
    var inW  = parseInt(d.ac_in_w)  || 0;
    var outW = parseInt(d.ac_out_w) || 0;
    var battV = parseFloat(d.batt_v) || 0;
    var battA = parseFloat(d.batt_a) || 0;
    var battW = Math.round(battV * battA);
    if (rm) rm.textContent = inNa  ? '--' : fmtW(inW);
    if (rl) rl.textContent = outNa ? '--' : fmtW(outW);
    if (rb) rb.textContent = fmtW(battW);
    if (fm) {
        var fmOn = !inNa && Math.abs(inW) > 5;
        fm.classList.toggle('active', fmOn);
        fm.classList.toggle('flow-r', fmOn && inW > 0);
        fm.classList.toggle('flow-l', fmOn && inW < 0);
    }
    if (fl) {
        var flOn = !outNa && Math.abs(outW) > 5;
        fl.classList.toggle('active', flOn);
        fl.classList.toggle('flow-r', flOn);
        fl.classList.remove('flow-l');
    }
    if (fb) {
        var fbOn = Math.abs(battW) > 5;
        fb.classList.toggle('active', fbOn);
        fb.classList.toggle('flow-r', fbOn && battW > 0);
        fb.classList.toggle('flow-l', fbOn && battW < 0);
    }

    var acOn = (parseInt(d.ac_in_state) || 0) < 2;
    if (mp) {
        mp.classList.toggle('ac-on', acOn);
        var mh = mp.querySelector('.inv-port-hdr');
        if (mh) mh.innerHTML = t('inv_mains') + (acOn ? ' <span class="fi">\ue55f</span>' : '');
    }
    var loadOn = Math.abs(outW) > 5;
    if (lp) lp.classList.toggle('load-on', loadOn);
    var charging = battW > 5;
    var discharging = battW < -5;
    if (bp) {
        bp.classList.toggle('batt-chg', charging);
        bp.classList.toggle('batt-dis', discharging);
        var bh = bp.querySelector('.inv-port-hdr');
        if (bh) {
            if (charging)
                bh.innerHTML = t('inv_batt') + ' <span class="fi">\uf178</span>';
            else if (discharging)
                bh.innerHTML = '<span class="fi">\uf177</span> ' + t('inv_batt');
            else
                bh.textContent = t('inv_batt');
        }
    }
}

function applyBatt(d) {
    var socLbl = document.getElementById('batt_soc');
    var fill   = document.getElementById('batt_fill');
    var bpw    = document.getElementById('batt_pwr_w');
    var bpp    = document.getElementById('batt_pwr_port');
    var bph    = document.getElementById('batt_pwr_hdr');
    var bfl    = document.getElementById('batt_flow');
    if (!socLbl) return;

    if (!d.valid) {
        socLbl.textContent = '--%';
        if (fill) fill.style.height = '0%';
        if (bpw) bpw.textContent = '--';
        if (bpp) { bpp.classList.remove('batt-chg'); bpp.classList.remove('batt-dis'); }
        if (bfl) { bfl.classList.remove('active', 'flow-r', 'flow-l'); }
        return;
    }
    var soc = parseInt(d.soc) || 0;
    socLbl.textContent = soc + '%';

    if (fill) {
        fill.style.height = Math.min(100, Math.max(0, soc)) + '%';
        var col = soc >= 50 ? '#44bb44' : soc >= 20 ? '#ffbb00' : '#ff3333';
        fill.style.background = col;
    }

    var battV = parseFloat(d.battV) || 0;
    var battA = parseFloat(d.battA) || 0;
    var battW = Math.round(battV * battA);
    var charging = battW > 5;
    var discharging = battW < -5;

    if (bpw) bpw.textContent = fmtW(battW);
    if (bfl) {
        var bfOn = Math.abs(battW) > 5;
        bfl.classList.toggle('active', bfOn);
        bfl.classList.toggle('flow-l', bfOn && charging);
        bfl.classList.toggle('flow-r', bfOn && discharging);
    }
    if (bpp) {
        bpp.classList.toggle('batt-chg', charging);
        bpp.classList.toggle('batt-dis', discharging);
    }
    if (bph) {
        if (charging)
            bph.innerHTML = '<span class="fi"></span> ' + t('batt_charge');
        else if (discharging)
            bph.innerHTML = t('batt_discharge') + ' <span class="fi"></span>';
        else
            bph.textContent = t('batt_charge');
    }
}

// ── Initialisation ────────────────────────────────────────────────────────
refreshSetpoint();
refreshHeat();
refreshBoiler();
