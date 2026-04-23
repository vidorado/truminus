'use strict';

// ── WebSocket ─────────────────────────────────────────────────────────────
var gateway = 'ws://' + window.location.hostname +
              (window.location.port ? ':' + window.location.port : '') + '/ws';
var wserror  = true;
var linerror = false;

// ── Estado de la aplicación ───────────────────────────────────────────────
var s_temp     = 20.0;   // setpoint temperatura habitación
var s_heat     = false;  // calefacción encendida
var s_boiler   = 'off';  // off | eco | high | boost
var s_fan      = 'off';  // eco | high | off | '1'-'10'
var s_energy   = 'gas';  // gas | electricity | mix
var s_elpower  = 0;      // 0 | 900 | 1800
var s_fanLevel = 5;      // último nivel numérico de ventilador (1-10)

// ── Modal ─────────────────────────────────────────────────────────────────
var modal      = document.getElementById('modal');
var modal_text = document.getElementById('modal-text');

function showModal() {
    if (wserror) {
        modal_text.textContent = 'Conectando, espera\u2026';
        modal.style.display = 'flex';
    } else if (linerror) {
        modal_text.textContent = 'Error LIN \u2013 Truma no responde';
        modal.style.display = 'flex';
    } else {
        modal.style.display = 'none';
    }
    var msg = document.getElementById('statusMsg');
    if (!msg) return;
    if      (wserror)  { msg.textContent = '\u26a0 Conectando\u2026'; msg.style.color = '#ffaa00'; }
    else if (linerror) { msg.textContent = '\u26a0 Sin LIN bus';      msg.style.color = '#ff4444'; }
    else               { msg.textContent = ''; }
}
showModal();

// ── WebSocket ─────────────────────────────────────────────────────────────
var ws = new ReconnectingWebSocket(gateway);
function ping() { ws.send('ping'); setTimeout(ping, 10000); }
setTimeout(ping, 10000);

ws.onopen = function () {
    wserror = false;
    showModal();
    setDot('dot-wifi', 'ok');
    ws.send('settings');
};
ws.onclose = function () {
    wserror = true;
    showModal();
    setDot('dot-wifi', 'err');
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

// ── Dot de estado (WiFi / LIN) ────────────────────────────────────────────
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
    } else if (id === 'energy') {
        s_energy = value;
        refreshEnergy();
    } else if (id === 'elpower') {
        s_elpower = parseInt(value) || 0;
        refreshEnergy();
    }
}

// ── Status recibido del dispositivo ──────────────────────────────────────
function applyStatus(id, value) {
    // Actualizar spans con id coincidente (room_temp, water_temp, current_state…)
    var el = document.getElementById(id);
    if (el) el.textContent = value;

    if (id === 'linok') {
        linerror = parseInt(value) !== 1;
        showModal();
        setDot('dot-lin', linerror ? 'err' : 'ok');
    }

    if (id === 'error_code') {
        var code = parseInt(value);
        var line = document.getElementById('error_line');
        if (code === 0) {
            line.textContent = '';
        } else {
            var desc = (typeof errors !== 'undefined' && errors[code])
                        ? errors[code] : 'C\u00f3digo ' + value;
            line.textContent = '\u26a0 ' + desc;
        }
    }

    // Tabla de diagnóstico: actualizar fila existente o añadir nueva
    var table = document.getElementById('detailsTable');
    for (var i = 1; i < table.rows.length; i++) {
        if (table.rows[i].cells[0].textContent === id) {
            table.rows[i].cells[1].textContent = value;
            return;
        }
    }
    var row = table.insertRow(-1);
    row.insertCell(0).textContent = id;
    row.insertCell(1).textContent = value;
}

// ── Funciones de refresco de UI ───────────────────────────────────────────

function cls(id, cssClass, on) {
    var el = document.getElementById(id);
    if (el) el.classList.toggle(cssClass, on);
}

function refreshSetpoint() {
    document.getElementById('spVal').textContent = s_temp.toFixed(1) + '\u00a0\u00b0C';
}

function refreshHeat() {
    var btn = document.getElementById('heatBtn');
    btn.textContent = s_heat ? 'ENCENDIDO' : 'APAGADO';
    btn.classList.toggle('btn-heat-on', s_heat);
    btn.classList.toggle('btn-off',    !s_heat);

    // Setpoint visible sólo con calefacción ON
    cls('spRow',      'vis-hidden', !s_heat);
    // Fila ventilador: alternar entre modo calefacción y modo standby
    cls('fanHeatRow', 'vis-hidden', !s_heat);
    cls('fanSbyRow',  'vis-hidden',  s_heat);
    if (s_heat) cls('fanLvlRow', 'vis-hidden', true);

    refreshFan();
}

function refreshFan() {
    if (s_heat) {
        // Modo calefacción: Eco | Alto | Apag.
        cls('fhEco',  'btn-sel', s_fan === 'eco');
        cls('fhHigh', 'btn-sel', s_fan === 'high');
        cls('fhOff',  'btn-sel', s_fan === 'off');
    } else {
        // Modo standby: On | Off + nivel
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

function refreshEnergy() {
    var idx = 0;
    if      (s_energy === 'mix')         idx = (s_elpower >= 1800) ? 2 : 1;
    else if (s_energy === 'electricity') idx = (s_elpower >= 1800) ? 4 : 3;
    document.getElementById('energyCombo').value = idx;
}

// ── Acciones del usuario ──────────────────────────────────────────────────

// Temperatura: se aplica con 400 ms de debounce para no saturar el WS
var tempTimer;
function changeTemp(delta) {
    clearTimeout(tempTimer);
    s_temp = Math.round((s_temp + delta) * 2) / 2;
    if (s_temp < 5)  s_temp = 5;
    if (s_temp > 30) s_temp = 30;
    refreshSetpoint();
    tempTimer = setTimeout(function () { send('/temp', s_temp.toFixed(1)); }, 400);
}

// Toggle calefacción
function toggleHeating() {
    s_heat = !s_heat;
    if (!s_heat) { s_fan = 'off'; send('/fan', 'off'); }
    send('/heating', s_heat ? '1' : '0');
    refreshHeat();
}

// Ventilador modo calefacción (eco / high / off)
function setFan(value) {
    s_fan = value;
    send('/fan', value);
    refreshFan();
}

// Ventilador modo standby (on activa el último nivel, off apaga)
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

// Ajuste de nivel de ventilador 1-10
function changeFanLvl(delta) {
    s_fanLevel = Math.min(10, Math.max(1, s_fanLevel + delta));
    if (s_fan !== 'off') {
        s_fan = String(s_fanLevel);
        send('/fan', s_fan);
    }
    document.getElementById('fanLvlVal').textContent = s_fanLevel;
}

// Modo de caldera
function setBoiler(value) {
    s_boiler = value;
    send('/boiler', value);
    refreshBoiler();
}

// Combo energía → envía /energy y /elpower por separado
var energyMap = [
    { e: 'gas',         p: '0'    },
    { e: 'mix',         p: '900'  },
    { e: 'mix',         p: '1800' },
    { e: 'electricity', p: '900'  },
    { e: 'electricity', p: '1800' }
];
function setEnergyCombo(idx) {
    var m = energyMap[parseInt(idx)];
    if (!m) return;
    s_energy  = m.e;
    s_elpower = parseInt(m.p);
    send('/energy',  m.e);
    send('/elpower', m.p);
}

// Portal de configuración WiFi
function openPortal() {
    if (!confirm('El dispositivo abrir\u00e1 el portal de configuraci\u00f3n y dejar\u00e1 de responder.\n' +
                 'Con\u00e9ctate a la red Wi-Fi \u201cTruMinus-Setup\u201d y ve a 192.168.4.1')) return;
    send('/portal', '1');
}

// ── Inicialización ────────────────────────────────────────────────────────
refreshSetpoint();
refreshHeat();
refreshBoiler();
refreshEnergy();
