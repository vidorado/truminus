#include "i18n.hpp"
#include "nvs.h"

static Language s_language    = Language::ES;
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
    "Monitorizaci\xC3\xB3n",    // SOLAR_CFG (label kept for compat — screen retitled)
    "Config. Monitorizaci\xC3\xB3n", // SOLAR_TITLE
    "Direcc. MAC",              // SOLAR_ADDR
    "Clave cifrado",            // SOLAR_KEY
    "Sin datos",                // SOLAR_NO_DATA
    "Volt.:",                   // SOLAR_VOLT
    "Carga:",                   // SOLAR_LOAD
    "Prod.:",                   // SOLAR_PROD
    "Toca lo m\xC3\xA1s cerca del centro del s\xC3\xADmbolo +",
    "Calibraci\xC3\xB3n completada!",
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
    "TruMinus - Monitorizaci\xC3\xB3n",
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
    "%d/3  Toca el centro de la cruz",
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
    "Configura tu servidor Plesk (Node.js) en la URL de arriba "
    "y pega el mismo token que TUNNEL_TOKEN.",                       // TUNNEL_INSTR
    "Conectado",                                                     // TUNNEL_STATUS_ON
    "Desconectado",                                                  // TUNNEL_STATUS_OFF
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
    "Monitoring",
    "Monitoring Config",
    "MAC Address",
    "Enc. Key",
    "No data",
    "Volt.:",
    "Charge:",
    "Yield:",
    "Tap as close as possible to the center of the + symbol",
    "Calibration complete!",
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
    "TruMinus - Monitoring",
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
    "%d/3  Tap the center of the cross",
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
    "Set up the Plesk (Node.js) bridge at the URL above and paste "
    "the same value as its TUNNEL_TOKEN env var.",                   // TUNNEL_INSTR
    "Connected",                                                     // TUNNEL_STATUS_ON
    "Disconnected",                                                  // TUNNEL_STATUS_OFF
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
        s_language    = Language::ES;
    } else {
        s_language    = (val == (uint8_t)Language::EN) ? Language::EN : Language::ES;
        s_languageSet = true;
    }
}
