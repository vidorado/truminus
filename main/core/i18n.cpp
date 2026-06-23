#include "i18n.hpp"
#include "nvs.h"

static Language s_language    = Language::EN;
static bool     s_languageSet = false;

// NVS location matches the table in CLAUDE.md: namespace "display", key "lang".
// Values: 0=ES  1=EN

static const char* ES_STRINGS[(int)TK::_COUNT] = {
    "CALEFACCI\xC3\x93N",        // HEATING
    "ENCENDIDO",                // HEAT_ON
    "APAGADO",                  // HEAT_OFF
    "VENTILADOR",               // FAN
    "Eco",                      // FAN_ECO
    "Alto",                     // FAN_HIGH
    "Apag.",                    // FAN_OFF
    "Enc.",                     // ON
    "Apag.",                    // OFF
    "AGUA CALIENTE",            // HOT_WATER
    "ENERGIA",                  // ENERGY
    "CONFIGURACI\xC3\x93N",     // SETTINGS
    "WiFi",                     // WIFI_CFG
    "MQTT",                     // MQTT_CFG
    "Pantalla",                 // DISP_CFG
    "Idioma",                   // LANGUAGE
    "Conf.",                    // CONF
    "Volver",                   // BACK
    "TruMinus - Config. Pantalla", // DISPLAY_TITLE
    "Apagado de pantalla",      // TIMEOUT_LABEL
    "30 segundos",              // TIMEOUT_30S
    "1 minuto",                 // TIMEOUT_1M
    "3 minutos",                // TIMEOUT_3M
    "No apagar",                // TIMEOUT_NEVER
    "TruMinus - Config. Idioma", // SELECT_LANG
    "Iniciando...",             // STATUS_INIT
    "Conectando WiFi...",       // STATUS_WIFI_CONN
    "Sin WiFi",                 // STATUS_NO_WIFI
    "Sin MQTT",                 // STATUS_NO_MQTT
    "Sin LIN bus",              // STATUS_NO_LIN
    "Actualizando firmware...", // STATUS_OTA
    "AVISO",                    // WARN_LBL
    "ERROR",                    // ERROR_LBL
    "BLOQUEADO",                // LOCKED_LBL
    "Clase %02Xh  /  Cod.%d",  // ERR_SUBTITLE_FMT
    "Aceptar",                  // ACCEPT
    "Perif\xC3\xA9ricos",        // SOLAR_CFG
    "Perif\xC3\xA9ricos / Peripherals", // SOLAR_TITLE
    "Direcc. MAC",              // SOLAR_ADDR
    "Clave cifrado",            // SOLAR_KEY
    "Sin datos",                // SOLAR_NO_DATA
    "Volt.:",                   // SOLAR_VOLT
    "Carga:",                   // SOLAR_LOAD
    "Prod.:",                   // SOLAR_PROD
    "TruMinus - Config. WiFi",
    "Red WiFi:",
    "Contrase\xC3\xB1" "a:",
    "contrase\xC3\xB1" "a...",
    "Conectar",
    "Conectando...",
    "Cancelar",
    "Omitir",
    "Guardar",
    "TruMinus - Config. MQTT",
    "Broker:",
    "Puerto:",
    "Usuario (opcional):",
    "usuario...",
    "contrase\xC3\xB1" "a (opcional)...",
    "Introduce los datos del broker MQTT",
    "TruMinus - Perif\xC3\xA9ricos",
    "Bater\xC3\xAD" "a Ultimatron",
    "MAC BLE (12 hex, sin \":\"): ",
    "Clave cifrado (32 hex):",
    "Bater\xC3\xAD" "a: opcional. Dejar vac\xC3\xADo si no hay bater\xC3\xAD" "a Ultimatron.",
    "12100AE21001 (opcional)",
    "Buscar dispositivos Victron",
    "Buscar bater\xC3\xAD" "a BLE (Ultimatron)",
    "Buscar sensor de dep\xC3\xB3sito (BTHome)",                       // SCAN_TANK
    "Sensor dep\xC3\xB3sito agua",                                     // TANK_SECTION
    "BLE BTHome (moisture 0x2F). Opcional.",                          // TANK_INFO
    "Inversor Multiplus (VE.Bus)",                                     // MULTI_SECTION
    "Necesita el dongle VE.Bus Smart. Opcional.",                      // MULTI_INFO
    "Buscando... 8 s",
    "No se encontraron dispositivos BLE",
    "Buscar",
    "Buscando redes WiFi...",
    "No se encontraron redes.",
    "Conectando a %s...",
    "Introduce la IP o nombre del broker",
    "Introduce el puerto (por defecto: 1883)",
    "Configuraci\xC3\xB3n guardada",
    "Buscando... %u s",
    "No se encontraron dispositivos Victron",
    "Clave cifrado Victron (32 hex sin \":\"):",
    "%d dispositivo(s) - toca para seleccionar",
    "(sin nombre)",
    "Brillo de pantalla",
    "Apagado",
    "Baja potencia",
    "Aver\xC3\xAD" "a",
    "Carga",
    "Absorci\xC3\xB3n",
    "Flotaci\xC3\xB3n",
    // Multiplus / VE.Bus
    "Almac.",                       // MULTI_STATE_STORAGE
    "Ecualiz.",                     // MULTI_STATE_EQUALIZE
    "Bypass",                       // MULTI_STATE_PASSTHRU
    "Invirtiendo",                  // MULTI_STATE_INVERTING
    "Asist. red",                   // MULTI_STATE_ASSIST
    "Suministro",                   // MULTI_STATE_SUPPLY
    "Ctrl ext.",                    // MULTI_STATE_EXT_CTRL
    // Tunnel WSS
    "T\xC3\xBAnel",                                                  // TUNNEL_CFG
    "TruMinus - T\xC3\xBAnel WS",                                    // TUNNEL_TITLE
    "Habilitar t\xC3\xBAnel",                                        // TUNNEL_ENABLE
    "Dominio",                                                       // TUNNEL_SERVER
    "tunel.tudominio.com",                                           // TUNNEL_SERVER_PH
    "Token",                                                         // TUNNEL_TOKEN
    "secreto compartido",                                            // TUNNEL_TOKEN_PH
    "Contrase\xC3\xB1" "a web (usuario: truminus)",                  // TUNNEL_PASS
    "vac\xC3\xAD" "a = sin contrase\xC3\xB1" "a",                    // TUNNEL_PASS_PH
    "Configura tu servidor Plesk (Node.js) en la URL de arriba "
    "y pega el mismo token que TUNNEL_TOKEN.",                       // TUNNEL_INSTR
    "Conectado",                                                     // TUNNEL_STATUS_ON
    "Desconectado",                                                  // TUNNEL_STATUS_OFF
    "AGUA LIMPIA",                                                   // FRESH_WATER
    "INVERSOR",                                                      // INVERTER
    "RED",                                                           // INV_MAINS
    "CARGAS",                                                        // INV_LOADS
    "CARGA",                                                         // BATT_CHARGE
    "DESCARGA",                                                      // BATT_DISCHARGE
    "Actualizaciones",                                               // UPDATES_CFG
    "TruMinus - Actualizaciones",                                    // UPDATES_TITLE
    "Versi\xC3\xB3n actual",                                         // OTA_CURRENT
    "\xC3\x9Altima versi\xC3\xB3n",                                  // OTA_LATEST
    "Est\xC3\xA1s al d\xC3\xAD" "a",                                 // OTA_UP_TO_DATE
    "Actualizaci\xC3\xB3n disponible",                               // OTA_AVAILABLE
    "Buscar",                                                        // OTA_CHECK
    "Comprobando\xE2\x80\xA6",                                       // OTA_CHECKING
    "Actualizar",                                                    // OTA_UPDATE_NOW
    "Actualizando\xE2\x80\xA6",                                      // OTA_UPDATING
    "Error al comprobar",                                            // OTA_CHECK_FAILED
    "Iniciando\xE2\x80\xA6 (%us)",                                   // OTA_STARTING
    "Sin comprobar",                                                 // OTA_NOT_CHECKED
    "Validando actualizaci\xC3\xB3n\xE2\x80\xA6",                    // OTA_VERIFYING
    "M\xC3\xA1s tarde",                                              // OTA_LATER
    "Comprobaci\xC3\xB3n autom\xC3\xA1tica",                         // OTA_AUTOCHECK
    "\xC2\xBF" "Actualizar ahora?",                                  // OTA_PROMPT
    "Error al actualizar",                                           // OTA_FAILED
    "Actualizando web UI\xE2\x80\xA6",                               // OTA_WEB_UPDATING
    "Error al actualizar web",                                       // OTA_WEB_FAILED
    "Actualizando firmware",                                         // OTA_FW_UPDATING
    "No apagues el dispositivo",                                     // OTA_NO_POWER_OFF
    "hoy",                                                           // TODAY
    "CLIMATIZACI\xC3\x93N",                                          // CLIMATE
    "AC OpenAIR Plus Bergstrom",                                     // AC_SECTION
    "Buscar AC OpenAir",                                             // SCAN_AC
    "BLE \"My OpenAir PLUS\". Opcional.",                            // AC_INFO
    "Auto",                                                          // FAN_AUTO
    "Man",                                                           // FAN_MAN
    "A/C: Error electroventilador",                                  // AC_ERR_EFAN
    "A/C: Error ventilador",                                         // AC_ERR_BLOWER
    "A/C: Error sonda antihielo",                                    // AC_ERR_FREEZE
    "A/C: Error de inclinaci\xC3\xB3n",                              // AC_ERR_TILT
    "A/C: Error compuerta 1",                                        // AC_ERR_FLAP1
    "A/C: Error compuerta 2",                                        // AC_ERR_FLAP2
    "El aire acondicionado no puede funcionar hasta resolver la aver\xC3\xAD" "a. Acude al servicio t\xC3\xA9" "cnico m\xC3\xA1s cercano.",  // AC_DESC_STOP
    "El veh\xC3\xAD" "culo tiene una inclinaci\xC3\xB3n excesiva. El equipo volver\xC3\xA1 a funcionar al nivelarse.",  // AC_DESC_TILT
    "Problema en la compuerta. El equipo puede seguir funcionando con operaci\xC3\xB3n limitada. Acude al servicio t\xC3\xA9" "cnico.",  // AC_DESC_FLAP
};

static const char* EN_STRINGS[(int)TK::_COUNT] = {
    "HEATING",
    "ON",
    "OFF",
    "FAN",
    "Eco",
    "High",
    "Off.",
    "On.",
    "Off.",
    "HOT WATER",
    "ENERGY",
    "SETTINGS",
    "WiFi",
    "MQTT",
    "Display",
    "Language",
    "Conf.",
    "Back",
    "TruMinus - Display Config",
    "Auto screen-off",
    "30 seconds",
    "1 minute",
    "3 minutes",
    "Never",
    "TruMinus - Language Config",
    "Starting...",
    "Connecting WiFi...",
    "No WiFi",
    "No MQTT",
    "No LIN bus",
    "Updating firmware...",
    "WARNING",
    "ERROR",
    "LOCKED",
    "Class %02Xh  /  Code %d",
    "Accept",
    "Peripherals",
    "Peripherals / Periféricos",
    "MAC Address",
    "Enc. Key",
    "No data",
    "Volt.:",
    "Charge:",
    "Yield:",
    "TruMinus - WiFi Setup",
    "WiFi Network:",
    "Password:",
    "password...",
    "Connect",
    "Connecting...",
    "Cancel",
    "Skip",
    "Save",
    "TruMinus - MQTT Setup",
    "Broker:",
    "Port:",
    "User (optional):",
    "user...",
    "password (optional)...",
    "Enter MQTT broker details",
    "TruMinus - Peripherals",
    "Ultimatron Battery",
    "MAC BLE (12 hex, no \":\"):",
    "Encryption Key (32 hex):",
    "Battery: optional. Leave empty if no Ultimatron battery.",
    "12100AE21001 (optional)",
    "Scan Victron devices",
    "Scan BLE battery (Ultimatron)",
    "Scan tank sensor (BTHome)",                                       // SCAN_TANK
    "Water tank sensor",                                               // TANK_SECTION
    "BLE BTHome (moisture 0x2F). Optional.",                          // TANK_INFO
    "Multiplus inverter (VE.Bus)",                                     // MULTI_SECTION
    "Requires the VE.Bus Smart dongle. Optional.",                     // MULTI_INFO
    "Scanning... 8 s",
    "No BLE devices found",
    "Search",
    "Scanning WiFi networks...",
    "No networks found.",
    "Connecting to %s...",
    "Enter the broker IP or name",
    "Enter the port (default: 1883)",
    "Configuration saved",
    "Scanning... %u s",
    "No Victron devices found",
    "Victron Encryption Key (32 hex no \":\"):",
    "%d device(s) - tap to select",
    "(unnamed)",
    "Screen brightness",
    "Off",
    "Low power",
    "Fault",
    "Bulk",
    "Absorption",
    "Float",
    // Multiplus / VE.Bus
    "Storage",                      // MULTI_STATE_STORAGE
    "Equalize",                     // MULTI_STATE_EQUALIZE
    "Passthru",                     // MULTI_STATE_PASSTHRU
    "Inverting",                    // MULTI_STATE_INVERTING
    "Power Assist",                 // MULTI_STATE_ASSIST
    "Power Supply",                 // MULTI_STATE_SUPPLY
    "External Ctrl",                // MULTI_STATE_EXT_CTRL
    // Tunnel WSS
    "Tunnel",                                                        // TUNNEL_CFG
    "TruMinus - WS Tunnel",                                          // TUNNEL_TITLE
    "Enable tunnel",                                                 // TUNNEL_ENABLE
    "Domain",                                                        // TUNNEL_SERVER
    "tunnel.yourdomain.com",                                         // TUNNEL_SERVER_PH
    "Token",                                                         // TUNNEL_TOKEN
    "shared secret",                                                 // TUNNEL_TOKEN_PH
    "Web password (user: truminus)",                                 // TUNNEL_PASS
    "empty = no password",                                           // TUNNEL_PASS_PH
    "Set up the Plesk (Node.js) bridge at the URL above and paste "
    "the same value as its TUNNEL_TOKEN env var.",                   // TUNNEL_INSTR
    "Connected",                                                     // TUNNEL_STATUS_ON
    "Disconnected",                                                  // TUNNEL_STATUS_OFF
    "FRESH WATER",                                                   // FRESH_WATER
    "INVERTER",                                                      // INVERTER
    "SHORE",                                                         // INV_MAINS
    "LOADS",                                                         // INV_LOADS
    "CHARGE",                                                        // BATT_CHARGE
    "DISCHARGE",                                                     // BATT_DISCHARGE
    "Updates",                                                       // UPDATES_CFG
    "TruMinus - Updates",                                            // UPDATES_TITLE
    "Current version",                                               // OTA_CURRENT
    "Latest version",                                                // OTA_LATEST
    "Up to date",                                                    // OTA_UP_TO_DATE
    "Update available",                                              // OTA_AVAILABLE
    "Check",                                                         // OTA_CHECK
    "Checking\xE2\x80\xA6",                                          // OTA_CHECKING
    "Update",                                                        // OTA_UPDATE_NOW
    "Updating\xE2\x80\xA6",                                          // OTA_UPDATING
    "Check failed",                                                  // OTA_CHECK_FAILED
    "Starting\xE2\x80\xA6 (%us)",                                    // OTA_STARTING
    "Not checked yet",                                               // OTA_NOT_CHECKED
    "Verifying update\xE2\x80\xA6",                                  // OTA_VERIFYING
    "Later",                                                         // OTA_LATER
    "Auto-check",                                                    // OTA_AUTOCHECK
    "Update now?",                                                   // OTA_PROMPT
    "Update failed",                                                 // OTA_FAILED
    "Updating web UI\xE2\x80\xA6",                                   // OTA_WEB_UPDATING
    "Web update failed",                                             // OTA_WEB_FAILED
    "Updating firmware",                                             // OTA_FW_UPDATING
    "Do not power off",                                              // OTA_NO_POWER_OFF
    "today",                                                         // TODAY
    "CLIMATE",                                                       // CLIMATE
    "AC OpenAIR Plus Bergstrom",                                     // AC_SECTION
    "Scan OpenAir A/C",                                              // SCAN_AC
    "BLE \"My OpenAir PLUS\". Optional.",                            // AC_INFO
    "Auto",                                                          // FAN_AUTO
    "Man",                                                           // FAN_MAN
    "A/C: Electric-Fan Error",                                       // AC_ERR_EFAN
    "A/C: Blower Error",                                             // AC_ERR_BLOWER
    "A/C: Freeze Probe Error",                                       // AC_ERR_FREEZE
    "A/C: Tilt Error",                                               // AC_ERR_TILT
    "A/C: Flap 1 Error",                                             // AC_ERR_FLAP1
    "A/C: Flap 2 Error",                                             // AC_ERR_FLAP2
    "Your AC unit cannot work until this issue is solved. Please visit your nearest Technical Service Workshop.",  // AC_DESC_STOP
    "Your vehicle has an excessive tilt. The AC unit returns to normal operation when the vehicle is level.",      // AC_DESC_TILT
    "Problem with the flap. The AC unit can keep working with limited operation. Please visit your Technical Service.",  // AC_DESC_FLAP
};

const char* t(TK key) {
    int idx = (int)key;
    if (idx < 0 || idx >= (int)TK::_COUNT) return "";
    return (s_language == Language::EN) ? EN_STRINGS[idx] : ES_STRINGS[idx];
}

Language currentLanguage() { return s_language; }
bool     isLanguageSet()   { return s_languageSet; }

void setLanguage(Language lang) {
    s_language    = lang;
    s_languageSet = true;
    nvs_handle_t h;
    if (nvs_open("display", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "lang", (uint8_t)lang);
        nvs_commit(h);
        nvs_close(h);
    }
}

void loadLanguage() {
    nvs_handle_t h;
    if (nvs_open("display", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t val = 0xFF;
    nvs_get_u8(h, "lang", &val);
    nvs_close(h);
    if (val == 0xFF) {
        s_languageSet = false;
        s_language    = Language::EN;
    } else {
        s_language    = (val == (uint8_t)Language::EN) ? Language::EN : Language::ES;
        s_languageSet = true;
    }
}
