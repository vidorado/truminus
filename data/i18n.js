'use strict';

var LANG_STRINGS = {
    en: {
        solar_charge: 'SOLAR CHARGE',
        battery:      'BATTERY',
        sol_off:      'Off',
        sol_fault:    'Fault',
        sol_bulk:     'Bulk',
        sol_abs:      'Absorption',
        sol_float:    'Float',
        sol_equalize: 'Equalize',
        sol_nodata:   'No data',
        heating:      'HEATING',
        heat_on:      'ON',
        heat_off:     'OFF',
        fan:          'FAN',
        fan_eco:      'Eco',
        fan_high:     'High',
        fan_on:       'On',
        fan_off:      'Off',
        boiler_off:   'Off.',
        hot_water:    'HOT WATER',
        accept:       'Accept',
        ws_conn:      '⚠ Connecting…',
        ws_no_lin:    '⚠ No LIN bus',
        err_warn:     'WARNING',
        err_error:    'ERROR',
        err_locked:   'LOCKED',
    },
    es: {
        solar_charge: 'CARGA SOLAR',
        battery:      'BATERÍA',
        sol_off:      'Apag.',
        sol_fault:    'Fallo',
        sol_bulk:     'Carga',
        sol_abs:      'Absorción',
        sol_float:    'Flotación',
        sol_equalize: 'Ecualiz.',
        sol_nodata:   'Sin datos',
        heating:      'CALEFACCIÓN',
        heat_on:      'ENCENDIDO',
        heat_off:     'APAGADO',
        fan:          'VENTILADOR',
        fan_eco:      'Eco',
        fan_high:     'Alto',
        fan_on:       'On',
        fan_off:      'Off',
        boiler_off:   'Apag.',
        hot_water:    'AGUA CALIENTE',
        accept:       'Aceptar',
        ws_conn:      '⚠ Conectando…',
        ws_no_lin:    '⚠ Sin LIN bus',
        err_warn:     'AVISO',
        err_error:    'ERROR',
        err_locked:   'BLOQUEADO',
    }
};

var s_lang = 'es';

function t(key) {
    var tbl = LANG_STRINGS[s_lang] || LANG_STRINGS['es'];
    var val = tbl[key];
    if (val !== undefined) return val;
    val = LANG_STRINGS['es'][key];
    return (val !== undefined) ? val : key;
}

function applyLanguage(lang) {
    if (!LANG_STRINGS[lang]) return;
    s_lang = lang;
    document.querySelectorAll('[data-i18n]').forEach(function (el) {
        el.textContent = t(el.getAttribute('data-i18n'));
    });
    // Refresh dynamic UI elements that embed translatable strings
    if (typeof refreshHeat    === 'function') refreshHeat();
    if (typeof updateStatusBar === 'function') updateStatusBar();
    if (typeof updateErrorDisplay === 'function') updateErrorDisplay();
}
