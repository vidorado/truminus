'use strict';

// ── WebSocket ─────────────────────────────────────────────────────────────
var gateway = 'ws://' + window.location.hostname +
              (window.location.port ? ':' + window.location.port : '') + '/ws';
var wserror      = true;
var linerror     = false;
var s_ssid       = '';

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
    // Format: "SSID  IP" — matches the CYD physical status bar.
    var addr = window.location.hostname;
    var ipText = s_ssid ? (s_ssid + ' / ' + addr) : addr;
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

// ── WebSocket ─────────────────────────────────────────────────────────────
var ws = new ReconnectingWebSocket(gateway);
function ping() { ws.send('ping'); setTimeout(ping, 10000); }
setTimeout(ping, 10000);

// Debounce the "WS down" indicator: brief reconnect cycles (under 3 s)
// would otherwise flicker the WiFi dot and the "Connecting..." footer.
// The physical CYD display never shows transient drops, mirror that UX.
var s_wsDownTimer = null;
ws.onopen = function () {
    if (s_wsDownTimer) { clearTimeout(s_wsDownTimer); s_wsDownTimer = null; }
    wserror = false;
    updateStatusBar();
    setDot('dot-wifi', 'ok');
    ws.send('settings');
};
ws.onclose = function () {
    if (s_wsDownTimer) return;   // already counting down
    s_wsDownTimer = setTimeout(function () {
        s_wsDownTimer = null;
        wserror = true;
        updateStatusBar();
        setDot('dot-wifi', 'err');
        setDot('dot-lin',  'err');
    }, 3000);
};
ws.onmessage = function (event) {
    var d = JSON.parse(event.data);
    if (!d.command) return;
    if (d.command === 'solar') { applySolar(d); return; }
    if (d.command === 'batt')  { applyBatt(d);  return; }
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
        if (d.outdoor_temp !== undefined) applyStatus('outdoor_temp', d.outdoor_temp);
        if (d.energy_idx !== undefined)   applySetting('energy_idx', d.energy_idx);
        if (d.lang !== undefined)         applySetting('lang', d.lang);
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

    if (id === 'ssid') {
        s_ssid = value || '';
        updateStatusBar();
        return;
    }

    if (id === 'linok') {
        linerror = parseInt(value) !== 1;
        updateStatusBar();
        setDot('dot-lin', linerror ? 'err' : 'ok');
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
    var btn = document.getElementById('heatBtn');
    btn.textContent = s_heat ? t('heat_on') : t('heat_off');
    btn.classList.toggle('btn-heat-on', s_heat);
    btn.classList.toggle('btn-off',    !s_heat);
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
    var boilerOn = s_boiler !== 'off';
    var boilerSetTemp = { eco: 40, high: 60, boost: 60 };
    var wSet = boilerSetTemp[s_boiler] || 0;
    var wAtTemp = boilerOn && s_waterTemp !== null && s_waterTemp > 0 && s_waterTemp >= wSet - 1;
    var wDemand = boilerOn && s_waterDemand && !wAtTemp;
    cls('ind-tint', 'ind-on',     boilerOn && !wDemand);
    cls('ind-tint', 'ind-active', wDemand);

    var heatDemand = s_heat && s_roomTemp !== null && (s_roomTemp < s_temp - 0.3);
    cls('ind-fire', 'ind-on',     s_heat && !heatDemand);
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
        // Fixed 0-70 °C scale and absolute colour thresholds, independent
        // of boiler state. The bar reads the same temperature regardless
        // of setpoint, matching how a household water heater gauge reads.
        var WTEMP_SCALE = 70;
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
            body.style.borderColor = '#888888';
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
function toggleHeating() {
    s_heat = !s_heat;
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
        document.getElementById('solar_battV').textContent = '--';
        document.getElementById('solar_battA').textContent = '--';
        return;
    }
    stateLbl.textContent = t(SOLAR_STATES[d.state] || 'sol_off');
    document.getElementById('solar_pvW').textContent   = d.pvW;
    document.getElementById('solar_kWh').textContent   = d.kWh;
    document.getElementById('solar_battV').textContent = d.battV;
    document.getElementById('solar_battA').textContent = d.battA;
}

function applyBatt(d) {
    var socLbl = document.getElementById('batt_soc');
    var fill   = document.getElementById('batt_fill');
    if (!socLbl) return;

    if (!d.valid) {
        socLbl.textContent = '--%';
        if (fill) fill.style.height = '0%';
        return;
    }
    var soc = parseInt(d.soc) || 0;
    socLbl.textContent = soc + '%';

    if (fill) {
        fill.style.height = Math.min(100, Math.max(0, soc)) + '%';
        var col = soc >= 50 ? '#44bb44' : soc >= 20 ? '#ffbb00' : '#ff3333';
        fill.style.background = col;
    }
}

// ── Initialisation ────────────────────────────────────────────────────────
refreshSetpoint();
refreshHeat();
refreshBoiler();
