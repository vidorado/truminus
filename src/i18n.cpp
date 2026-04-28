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
    "Pantalla",                 // DISPLAY
    "Idioma",                   // LANGUAGE
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
    "Display",                  // DISPLAY
    "Language",                 // LANGUAGE
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
