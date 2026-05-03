#ifdef CYD
#include "i18n.hpp"
#include <Preferences.h>

static Language s_language    = Language::ES;
static bool     s_languageSet = false;

static const char* ES_STRINGS[(int)TK::_COUNT] = {
    "CALEFACCION",              // HEATING
    "ENCENDIDO",                // HEAT_ON
    "APAGADO",                  // HEAT_OFF
    "VENTILADOR",               // FAN
    "Eco",                      // FAN_ECO
    "Alto",                     // FAN_HIGH
    "Apag.",                    // FAN_OFF
    "On",                       // ON
    "Off",                      // OFF
    "AGUA CALIENTE",            // HOT_WATER
    "ENERGIA",                  // ENERGY
    "CONFIGURACION",            // SETTINGS
    "WiFi Config",              // WIFI_CFG
    "MQTT Config",              // MQTT_CFG
    "Pantalla",                 // DISP_CFG
    "Idioma",                   // LANGUAGE
    "Conf.",                    // CONF
    "Volver",                   // BACK
    "PANTALLA",                 // DISPLAY_TITLE
    "Apagado de pantalla",      // TIMEOUT_LABEL
    "30 segundos",              // TIMEOUT_30S
    "1 minuto",                 // TIMEOUT_1M
    "3 minutos",                // TIMEOUT_3M
    "No apagar",                // TIMEOUT_NEVER
    "IDIOMA",                   // SELECT_LANG
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
    "Carga solar",              // SOLAR_CFG
    "Config. Solar",            // SOLAR_TITLE
    "Direcc. MAC",              // SOLAR_ADDR
    "Clave cifrado",            // SOLAR_KEY
    "Sin datos",                // SOLAR_NO_DATA
    "Toca lo mas cerca del centro del simbolo +",   // TOUCH_CAL_INSTR
    "Calibracion completada!",                      // TOUCH_CAL_DONE
    "TruMinus - Configuracion WiFi",                // WIFI_TITLE
    "Red WiFi:",                                    // WIFI_NETWORK
    "Contrasena:",                                  // PASSWORD
    "contrasena...",                                // PASSWORD_PH
    "Conectar",                                     // CONNECT
    "Conectando...",                                // CONNECTING
    "Cancelar",                                     // CANCEL
    "Omitir",                                       // SKIP
    "Guardar",                                      // SAVE
    "TruMinus - Configuracion MQTT",                // MQTT_TITLE
    "Broker:",                                      // MQTT_BROKER
    "Puerto:",                                      // MQTT_PORT
    "Usuario (opcional):",                          // USER_OPT
    "usuario...",                                   // USER_PH
    "contrasena (opcional)...",                     // PASSWORD_OPT_PH
    "Introduce los datos del broker MQTT",          // MQTT_INSTR
    "TruMinus - Config. BLE",                       // BLE_TITLE
    "Bateria Ultimatron",                           // BATT_SECTION
    "MAC BLE (12 hex, sin \":\"):",                  // MAC_BLE_LABEL
    "Clave cifrado (32 hex):",                      // ENC_KEY_LABEL
    "Bateria: opcional. Dejar vacio si no hay bateria Ultimatron.", // BATT_INFO
    "12100AE21001 (opcional)",                      // BATT_MAC_PH
    "Buscar dispositivos Victron",                  // SCAN_VICTRON
    "Buscar bateria BLE (Ultimatron)",              // SCAN_BATT
    "Buscando... 8 s",                              // SCANNING
    "No se encontraron dispositivos BLE",           // NO_BLE_DEVS
    "Buscar",                                       // SEARCH
    "%d/3  Toca el centro de la cruz",              // TOUCH_CAL_STEP_FMT
    "Buscando redes WiFi...",                       // WIFI_SCANNING
    "No se encontraron redes.",                     // WIFI_NO_NETS
    "Conectando a %s...",                           // CONNECTING_TO_FMT
    "Introduce la IP o nombre del broker",          // MQTT_ENTER_BROKER
    "Introduce el puerto (por defecto: 1883)",      // MQTT_ENTER_PORT
    "Configuracion guardada",                       // CFG_SAVED
    "Buscando... %u s",                             // SCANNING_FMT
    "No se encontraron dispositivos Victron",       // NO_VICTRON_DEVS
    "Clave cifrado Victron (32 hex sin \":\"):",     // VICTRON_KEY_PROMPT
    "%d dispositivo(s) - toca para seleccionar",    // DEVICES_FOUND_FMT
    "(sin nombre)",                                 // UNNAMED
};

static const char* EN_STRINGS[(int)TK::_COUNT] = {
    "HEATING",                  // HEATING
    "ON",                       // HEAT_ON
    "OFF",                      // HEAT_OFF
    "FAN",                      // FAN
    "Eco",                      // FAN_ECO
    "High",                     // FAN_HIGH
    "Off.",                     // FAN_OFF
    "On",                       // ON
    "Off",                      // OFF
    "HOT WATER",                // HOT_WATER
    "ENERGY",                   // ENERGY
    "SETTINGS",                 // SETTINGS
    "WiFi Config",              // WIFI_CFG
    "MQTT Config",              // MQTT_CFG
    "Display",                  // DISP_CFG
    "Language",                 // LANGUAGE
    "Conf.",                    // CONF
    "Back",                     // BACK
    "DISPLAY",                  // DISPLAY_TITLE
    "Auto screen-off",          // TIMEOUT_LABEL
    "30 seconds",               // TIMEOUT_30S
    "1 minute",                 // TIMEOUT_1M
    "3 minutes",                // TIMEOUT_3M
    "Never",                    // TIMEOUT_NEVER
    "LANGUAGE",                 // SELECT_LANG
    "Starting...",              // STATUS_INIT
    "Connecting WiFi...",       // STATUS_WIFI_CONN
    "No WiFi",                  // STATUS_NO_WIFI
    "No MQTT",                  // STATUS_NO_MQTT
    "No LIN bus",               // STATUS_NO_LIN
    "Updating firmware...",     // STATUS_OTA
    "WARNING",                  // WARN_LBL
    "ERROR",                    // ERROR_LBL
    "LOCKED",                   // LOCKED_LBL
    "Class %02Xh  /  Code %d", // ERR_SUBTITLE_FMT
    "Accept",                   // ACCEPT
    "Solar charge",             // SOLAR_CFG
    "Solar Config",             // SOLAR_TITLE
    "MAC Address",              // SOLAR_ADDR
    "Enc. Key",                 // SOLAR_KEY
    "No data",                  // SOLAR_NO_DATA
    "Tap as close as possible to the center of the + symbol",  // TOUCH_CAL_INSTR
    "Calibration complete!",                        // TOUCH_CAL_DONE
    "TruMinus - WiFi Setup",                        // WIFI_TITLE
    "WiFi Network:",                                // WIFI_NETWORK
    "Password:",                                    // PASSWORD
    "password...",                                  // PASSWORD_PH
    "Connect",                                      // CONNECT
    "Connecting...",                                // CONNECTING
    "Cancel",                                       // CANCEL
    "Skip",                                         // SKIP
    "Save",                                         // SAVE
    "TruMinus - MQTT Setup",                        // MQTT_TITLE
    "Broker:",                                      // MQTT_BROKER
    "Port:",                                        // MQTT_PORT
    "User (optional):",                             // USER_OPT
    "user...",                                      // USER_PH
    "password (optional)...",                       // PASSWORD_OPT_PH
    "Enter MQTT broker details",                    // MQTT_INSTR
    "TruMinus - BLE Setup",                         // BLE_TITLE
    "Ultimatron Battery",                           // BATT_SECTION
    "MAC BLE (12 hex, no \":\"):",                   // MAC_BLE_LABEL
    "Encryption Key (32 hex):",                     // ENC_KEY_LABEL
    "Battery: optional. Leave empty if no Ultimatron battery.", // BATT_INFO
    "12100AE21001 (optional)",                      // BATT_MAC_PH
    "Scan Victron devices",                         // SCAN_VICTRON
    "Scan BLE battery (Ultimatron)",                // SCAN_BATT
    "Scanning... 8 s",                              // SCANNING
    "No BLE devices found",                         // NO_BLE_DEVS
    "Search",                                       // SEARCH
    "%d/3  Tap the center of the cross",            // TOUCH_CAL_STEP_FMT
    "Scanning WiFi networks...",                    // WIFI_SCANNING
    "No networks found.",                           // WIFI_NO_NETS
    "Connecting to %s...",                          // CONNECTING_TO_FMT
    "Enter the broker IP or name",                  // MQTT_ENTER_BROKER
    "Enter the port (default: 1883)",               // MQTT_ENTER_PORT
    "Configuration saved",                          // CFG_SAVED
    "Scanning... %u s",                             // SCANNING_FMT
    "No Victron devices found",                     // NO_VICTRON_DEVS
    "Victron Encryption Key (32 hex no \":\"):",     // VICTRON_KEY_PROMPT
    "%d device(s) - tap to select",                 // DEVICES_FOUND_FMT
    "(unnamed)",                                    // UNNAMED
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
    Preferences p;
    p.begin("lang", false);
    p.putUInt("lang_id", (uint32_t)lang);
    p.end();
}

void loadLanguage() {
    Preferences p;
    p.begin("lang", true);
    uint32_t stored = p.getUInt("lang_id", 0xFF);
    p.end();
    if (stored == 0xFF) {
        s_languageSet = false;
        s_language    = Language::ES;
    } else {
        s_language    = (Language)(stored & 1);
        s_languageSet = true;
    }
}

#endif // CYD
