'use strict';

// ── WebSocket ─────────────────────────────────────────────────────────────
var gateway = 'ws://' + window.location.hostname +
              (window.location.port ? ':' + window.location.port : '') + '/ws';
var wserror  = true;
var linerror = false;

// ── Estado de la aplicación ───────────────────────────────────────────────
var s_temp       = 20.0;
var s_heat       = false;
var s_boiler     = 'off';
var s_fan        = 'off';
var s_energyIdx  = 0;
var s_fanLevel   = 5;

// ── Barra de estado ───────────────────────────────────────────────────────
function updateStatusBar() {
    var msg = document.getElementById('statusMsg');
    var ip  = document.getElementById('deviceIp');
    if (!msg) return;
    if (wserror) {
        msg.textContent  = '⚠ Conectando…';
        msg.style.color  = '#ffaa00';
        if (ip) { ip.textContent = ''; }
    } else if (linerror) {
        msg.textContent  = '⚠ Sin LIN bus';
        msg.style.color  = '#ff4444';
        if (ip) { ip.textContent = window.location.hostname; }
    } else {
        msg.textContent  = '';
        msg.style.color  = '';
        if (ip) { ip.textContent = window.location.hostname; }
    }
}
updateStatusBar();

// ── WebSocket ─────────────────────────────────────────────────────────────
var ws = new ReconnectingWebSocket(gateway);
function ping() { ws.send('ping'); setTimeout(ping, 10000); }
setTimeout(ping, 10000);

ws.onopen = function () {
    wserror = false;
    updateStatusBar();
    setDot('dot-wifi', 'ok');
    ws.send('settings');
};
ws.onclose = function () {
    wserror = true;
    updateStatusBar();
    setDot('dot-wifi', 'err');
    setDot('dot-mqtt', 'dis');
    setDot('dot-lin',  'err');
};
ws.onmessage = function (event) {
    var d = JSON.parse(event.data);
    if (!d.command || !d.id) return;
    var id = d.id.replace(/^\//, '');
    if (d.command === 'setting') applySetting(id, d.value);
    else                         applyStatus(id, d.value);
};

// ── Envío al dispositivo ──────────────────────────────────────────────────
function send(id, value) {
    if (ws.readyState === 1)
        ws.send(JSON.stringify({ id: id, value: String(value) }));
}

// ── Dot de estado ─────────────────────────────────────────────────────────
function setDot(id, state) {
    var el = document.getElementById(id);
    if (el) el.className = 'sdot sdot-' + state;
}

// ── Settings recibidos del dispositivo ────────────────────────────────────
function applySetting(id, value) {
    if (id === 'temp') {
        s_temp = parseFloat(value) || 20;
        refreshSetpoint();
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
    } else if (id === 'energy_idx') {
        s_energyIdx = parseInt(value) || 0;
        refreshEnergyCombo();
    }
}

// ── Status recibido del dispositivo ──────────────────────────────────────
function applyStatus(id, value) {
    var el = document.getElementById(id);
    if (el) el.textContent = value;

    if (id === 'linok') {
        linerror = parseInt(value) !== 1;
        updateStatusBar();
        setDot('dot-lin', linerror ? 'err' : 'ok');
    }

    if (id === 'mqttok') {
        setDot('dot-mqtt', parseInt(value) === 1 ? 'ok' : 'err');
    }

    if (id === 'error_code') {
        var code = parseInt(value);
        var line = document.getElementById('error_line');
        if (code === 0) {
            line.textContent = '';
        } else {
            var desc = (typeof errors !== 'undefined' && errors[code])
                        ? errors[code] : 'Código ' + value;
            line.textContent = '⚠ ' + desc;
        }
    }
}

// ── Funciones de refresco de UI ───────────────────────────────────────────

function cls(id, cssClass, on) {
    var el = document.getElementById(id);
    if (el) el.classList.toggle(cssClass, on);
}

function refreshSetpoint() {
    document.getElementById('spVal').textContent = s_temp.toFixed(1) + ' °C';
}

function refreshHeat() {
    var btn = document.getElementById('heatBtn');
    btn.textContent = s_heat ? 'ENCENDIDO' : 'APAGADO';
    btn.classList.toggle('btn-heat-on', s_heat);
    btn.classList.toggle('btn-off',    !s_heat);
    cls('spRow',      'vis-hidden', !s_heat);
    cls('fanHeatRow', 'vis-hidden', !s_heat);
    cls('fanSbyRow',  'vis-hidden',  s_heat);
    if (s_heat) cls('fanLvlRow', 'vis-hidden', true);
    refreshFan();
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
}

function refreshEnergyCombo() {
    document.getElementById('energyCombo').value = s_energyIdx;
}

// ── Acciones del usuario ──────────────────────────────────────────────────

var tempTimer;
function changeTemp(delta) {
    clearTimeout(tempTimer);
    s_temp = Math.round((s_temp + delta) * 2) / 2;
    if (s_temp < 5)  s_temp = 5;
    if (s_temp > 30) s_temp = 30;
    refreshSetpoint();
    tempTimer = setTimeout(function () { send('/temp', s_temp.toFixed(1)); }, 400);
}

function toggleHeating() {
    s_heat = !s_heat;
    if (!s_heat) { s_fan = 'off'; send('/fan', 'off'); }
    send('/heating', s_heat ? '1' : '0');
    refreshHeat();
}

function setFan(value) {
    s_fan = value;
    send('/fan', value);
    refreshFan();
}

function setFanSby(mode) {
    if (mode === 'on') {
        s_fan = String(s_fanLevel);
        send('/fan', s_fan);
    } else {
        s_fan = 'off';
        send('/fan', 'off');
    }
    refreshFan();
}

function changeFanLvl(delta) {
    s_fanLevel = Math.min(10, Math.max(1, s_fanLevel + delta));
    if (s_fan !== 'off') {
        s_fan = String(s_fanLevel);
        send('/fan', s_fan);
    }
    document.getElementById('fanLvlVal').textContent = s_fanLevel;
}

function setBoiler(value) {
    s_boiler = value;
    send('/boiler', value);
    refreshBoiler();
}

function setEnergyCombo(idx) {
    s_energyIdx = parseInt(idx);
    send('/energy_idx', String(s_energyIdx));
    refreshEnergyCombo();
}

// ── Inicialización ────────────────────────────────────────────────────────
refreshSetpoint();
refreshHeat();
refreshBoiler();
refreshEnergyCombo();
