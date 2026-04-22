var gateway = 'ws://' + window.location.hostname + (window.location.port ? ':' + window.location.port : '') + '/ws';
var wserror  = true;
var linerror = false;
var modal      = document.getElementById('modal');
var modal_text = document.getElementById('modal-text');

function showModal() {
    if (wserror) {
        modal_text.textContent = 'Connecting, please wait';
        modal.style.display = 'flex';
    } else if (linerror) {
        modal_text.textContent = 'LIN bus error – Truma not responding';
        modal.style.display = 'flex';
    } else {
        modal.style.display = 'none';
    }
}
showModal();

// -----------------------------------------------------------------------
// WebSocket
// -----------------------------------------------------------------------
var ws = new ReconnectingWebSocket(gateway);

function ping() { ws.send('ping'); setTimeout(ping, 10000); }
setTimeout(ping, 10000);

ws.onopen = function() {
    wserror = false;
    showModal();
    ws.send('settings');
};
ws.onclose = function() {
    wserror = true;
    showModal();
};

ws.onmessage = function(event) {
    var data = JSON.parse(event.data);
    if (!data.command || !data.id) return;
    var id    = data.id.replace(/^\//, '');   // strip leading /
    var value = data.value;
    if (data.command === 'setting') {
        updateSetting(id, value);
    } else {
        updateStatus(id, value);
    }
};

// -----------------------------------------------------------------------
// Update a setpoint control
// -----------------------------------------------------------------------
function updateSetting(id, value) {
    var el = document.getElementById(id);
    if (!el) return;
    el.value = value;
    if (id === 'heating') { enableHeatingTemp(); enableFan(); }
    if (id === 'boiler') enableFan();
}

// -----------------------------------------------------------------------
// Update a status value
// -----------------------------------------------------------------------
function updateStatus(id, value) {
    // Inline status fields
    var el = document.getElementById(id);
    if (el) el.textContent = value;

    // LIN alive tracking
    if (id === 'linok') {
        linerror = parseInt(value) !== 1;
        showModal();
    }

    // Error code display
    if (id === 'error_code') {
        var line = document.getElementById('error_line');
        if (parseInt(value) === 0) {
            line.textContent = '';
        } else {
            line.textContent = 'Error code: ' + value;
        }
    }

    // Dynamic details table
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

// -----------------------------------------------------------------------
// Send a setpoint change to the device
// -----------------------------------------------------------------------
function send(id, value) {
    if (ws.readyState === 1) {
        ws.send(JSON.stringify({ id: id, value: String(value) }));
    }
}

// -----------------------------------------------------------------------
// Room temperature controls
// -----------------------------------------------------------------------
var tempTimer;

function applyTemp() {
    var el  = document.getElementById('temp');
    var val = parseFloat(el.value);
    if (val < 5.0)  val = 5.0;
    if (val > 30.0) val = 30.0;
    el.value = val.toFixed(1);
    send('/temp', el.value);
}

function changeTemp(delta) {
    clearTimeout(tempTimer);
    var el  = document.getElementById('temp');
    var val = parseFloat(el.value) + delta;
    if (val < 5.0)  val = 5.0;
    if (val > 30.0) val = 30.0;
    el.value = val.toFixed(1);
    tempTimer = setTimeout(applyTemp, 600);
}

document.getElementById('temp').addEventListener('input', function() {
    clearTimeout(tempTimer);
    tempTimer = setTimeout(applyTemp, 800);
});

// -----------------------------------------------------------------------
// Select controls
// -----------------------------------------------------------------------
document.getElementById('heating').addEventListener('change', function() {
    send('/heating', this.value);
    enableHeatingTemp();
    enableFan();
});
document.getElementById('boiler').addEventListener('change', function() {
    send('/boiler', this.value);
    enableFan();
});
document.getElementById('fan').addEventListener('change', function() {
    send('/fan', this.value);
});
document.getElementById('elpower').addEventListener('change', function() {
    send('/elpower', this.value);
});
document.getElementById('energy').addEventListener('change', function() {
    send('/energy', this.value);
});

// Room temp spinner enabled only when heating is On
function enableHeatingTemp() {
    var heatingOn = document.getElementById('heating').value === '1';
    var tempEl = document.getElementById('temp');
    tempEl.disabled = !heatingOn;
    document.querySelectorAll('.number-input button').forEach(function(b) {
        b.disabled = !heatingOn;
    });
}

// Fan enabled when heating is On OR boiler is Off
function enableFan() {
    var heatingOn = document.getElementById('heating').value === '1';
    var boilerOff = document.getElementById('boiler').value === 'off';
    document.getElementById('fan').disabled = !(heatingOn || boilerOff);
}

// -----------------------------------------------------------------------
// Config portal
// -----------------------------------------------------------------------
function openPortal() {
    if (!confirm('El dispositivo abrirá el portal de configuración y dejará de responder.\nConéctate a la red Wi-Fi "TruMinus-Setup" y ve a 192.168.4.1')) return;
    send('/portal', '1');
}

// -----------------------------------------------------------------------
// Tab switching
// -----------------------------------------------------------------------
window.openTab = function(evt, tabName) {
    var tabs = document.getElementsByClassName('tabcontent');
    for (var i = 0; i < tabs.length; i++) tabs[i].style.display = 'none';
    var links = document.getElementsByClassName('tablinks');
    for (var i = 0; i < links.length; i++) links[i].className = links[i].className.replace(' active', '');
    document.getElementById(tabName).style.display = 'block';
    evt.currentTarget.className += ' active';
};

document.querySelector('.tablinks.active').click();
enableHeatingTemp();
enableFan();
