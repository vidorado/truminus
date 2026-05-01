#pragma once
#ifdef CYD
#include <Arduino.h>

enum class Language : uint8_t { EN = 0, ES = 1 };

enum class TK : uint8_t {
    HEATING,           // panel title: "CALEFACCION" / "HEATING"
    HEAT_ON,           // heat button active: "ENCENDIDO" / "ON"
    HEAT_OFF,          // heat button inactive: "APAGADO" / "OFF"
    FAN,               // section label: "VENTILADOR" / "FAN"
    FAN_ECO,           // "Eco"
    FAN_HIGH,          // "Alto" / "High"
    FAN_OFF,           // abbreviated: "Apag." / "Off."
    ON,                // fan standby on button: "On"
    OFF,               // fan standby off button: "Off"
    HOT_WATER,         // panel title: "AGUA CALIENTE" / "HOT WATER"
    ENERGY,            // section label: "ENERGIA" / "ENERGY"
    SETTINGS,          // settings screen title: "CONFIGURACION" / "SETTINGS"
    WIFI_CFG,          // settings menu item: "WiFi Config"
    MQTT_CFG,          // settings menu item: "MQTT Config"
    DISP_CFG,          // settings menu item: "Pantalla" / "Display"
    LANGUAGE,          // settings menu item: "Idioma" / "Language"
    BACK,              // back button: "Volver" / "Back"
    DISPLAY_TITLE,     // display settings title: "PANTALLA" / "DISPLAY"
    TIMEOUT_LABEL,     // display settings subtitle: "Apagado de pantalla" / "Auto screen-off"
    TIMEOUT_30S,       // "30 segundos" / "30 seconds"
    TIMEOUT_1M,        // "1 minuto" / "1 minute"
    TIMEOUT_3M,        // "3 minutos" / "3 minutes"
    TIMEOUT_NEVER,     // "No apagar" / "Never"
    SELECT_LANG,       // language selection title: "IDIOMA" / "LANGUAGE"
    STATUS_INIT,       // "Iniciando..." / "Starting..."
    STATUS_WIFI_CONN,  // "Conectando WiFi..." / "Connecting WiFi..."
    STATUS_NO_WIFI,    // "Sin WiFi" / "No WiFi"
    STATUS_NO_MQTT,    // "Sin MQTT" / "No MQTT"
    STATUS_NO_LIN,     // "Sin LIN bus" / "No LIN bus"
    STATUS_OTA,        // "Actualizando firmware..." / "Updating firmware..."
    WARN_LBL,          // error modal: "AVISO" / "WARNING"
    ERROR_LBL,         // error modal: "ERROR"
    LOCKED_LBL,        // error modal: "BLOQUEADO" / "LOCKED"
    ERR_SUBTITLE_FMT,  // "Clase %02Xh  /  Cod.%d" / "Class %02Xh  /  Code %d"
    ACCEPT,            // modal button: "Aceptar" / "Accept"
    SOLAR_CFG,         // settings menu: "Solar"
    SOLAR_TITLE,       // solar config title: "Config. Solar" / "Solar Config"
    SOLAR_ADDR,        // solar setup label: "Direcc. MAC" / "MAC Address"
    SOLAR_KEY,         // solar setup label: "Clave cifrado" / "Enc. Key"
    SOLAR_NO_DATA,     // right panel when no data: "Sin datos" / "No data"
    _COUNT
};

const char*  t(TK key);
Language     currentLanguage();
bool         isLanguageSet();
void         setLanguage(Language lang);
void         loadLanguage();

#endif // CYD
