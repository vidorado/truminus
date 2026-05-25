#pragma once
#include <stdint.h>

enum class Language : uint8_t { ES = 0, EN = 1 };

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
    CONF,              // config button label: "Conf." / "Conf."
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
    SOLAR_VOLT,        // solar panel label: "Volt.:" / "Volt.:"
    SOLAR_LOAD,        // solar panel label: "Carga:" / "Load:"
    SOLAR_PROD,        // solar panel label: "Prod.:" / "Yield:"
    // ── Touch calibration ──────────────────────────────────────────────────
    TOUCH_CAL_INSTR,   // "Toca lo mas cerca del centro del simbolo +" / "Tap as close as possible to the center of the + symbol"
    TOUCH_CAL_DONE,    // "Calibracion completada!" / "Calibration complete!"
    // ── WiFi setup ─────────────────────────────────────────────────────────
    WIFI_TITLE,        // "TruMinus - Configuracion WiFi" / "TruMinus - WiFi Setup"
    WIFI_NETWORK,      // "Red WiFi:" / "WiFi Network:"
    PASSWORD,          // "Contrasena:" / "Password:"
    PASSWORD_PH,       // placeholder: "contrasena..." / "password..."
    CONNECT,           // "Conectar" / "Connect"
    CONNECTING,        // "Conectando..." / "Connecting..."
    CANCEL,            // "Cancelar" / "Cancel"
    SKIP,              // "Omitir" / "Skip"
    SAVE,              // "Guardar" / "Save"
    // ── MQTT setup ─────────────────────────────────────────────────────────
    MQTT_TITLE,        // "TruMinus - Configuracion MQTT" / "TruMinus - MQTT Setup"
    MQTT_BROKER,       // "Broker:"
    MQTT_PORT,         // "Puerto:" / "Port:"
    USER_OPT,          // "Usuario (opcional):" / "User (optional):"
    USER_PH,           // placeholder: "usuario..." / "user..."
    PASSWORD_OPT_PH,   // placeholder: "contrasena (opcional)..." / "password (optional)..."
    MQTT_INSTR,        // "Introduce los datos del broker MQTT" / "Enter MQTT broker details"
    // ── Solar / Battery BLE setup ──────────────────────────────────────────
    BLE_TITLE,         // "TruMinus - Config. BLE" / "TruMinus - BLE Setup"
    BATT_SECTION,      // "Bateria Ultimatron" / "Ultimatron Battery"
    MAC_BLE_LABEL,     // "MAC BLE (12 hex, sin \":\"):" / "MAC BLE (12 hex, no \":\"):"
    ENC_KEY_LABEL,     // "Clave cifrado (32 hex):" / "Encryption Key (32 hex):"
    BATT_INFO,         // "Bateria: opcional..." / "Battery: optional..."
    BATT_MAC_PH,       // placeholder: "12100AE21001 (opcional)" / "12100AE21001 (optional)"
    // ── BLE scan screens ───────────────────────────────────────────────────
    SCAN_VICTRON,      // "Buscar dispositivos Victron" / "Scan Victron devices"
    SCAN_BATT,         // "Buscar bateria BLE (Ultimatron)" / "Scan BLE battery (Ultimatron)"
    SCAN_TANK,         // "Buscar sensor de deposito (BTHome)" / "Scan tank sensor (BTHome)"
    TANK_SECTION,      // "Sensor deposito agua" / "Water tank sensor"
    TANK_INFO,         // "BLE BTHome moisture (0x2F). Opcional." / "BLE BTHome moisture (0x2F). Optional."
    MULTI_SECTION,     // "Inversor Multiplus (VE.Bus)" / "Multiplus inverter (VE.Bus)"
    MULTI_INFO,        // "Necesita el dongle VE.Bus Smart. Opcional." / "Requires the VE.Bus Smart dongle. Optional."
    SCANNING,          // "Buscando... 8 s" / "Scanning... 8 s"
    NO_BLE_DEVS,       // "No se encontraron dispositivos BLE" / "No BLE devices found"
    SEARCH,            // scan button: "Buscar" / "Search"
    // ── Touch calibration / WiFi scan / status messages ───────────────────
    TOUCH_CAL_STEP_FMT,    // "%d/3  Toca el centro de la cruz" / "%d/3  Tap the center of the cross"
    WIFI_SCANNING,         // "Buscando redes WiFi..." / "Scanning WiFi networks..."
    WIFI_NO_NETS,          // "No se encontraron redes." / "No networks found."
    CONNECTING_TO_FMT,     // "Conectando a %s..." / "Connecting to %s..."
    MQTT_ENTER_BROKER,     // "Introduce la IP o nombre del broker" / "Enter the broker IP or name"
    MQTT_ENTER_PORT,       // "Introduce el puerto (por defecto: 1883)" / "Enter the port (default: 1883)"
    CFG_SAVED,             // "Configuracion guardada" / "Configuration saved"
    SCANNING_FMT,          // "Buscando... %u s" / "Scanning... %u s"
    NO_VICTRON_DEVS,       // "No se encontraron dispositivos Victron" / "No Victron devices found"
    VICTRON_KEY_PROMPT,    // "Clave cifrado Victron (32 hex sin \":\"):" / "Victron Encryption Key (32 hex no \":\"):"
    DEVICES_FOUND_FMT,     // "%d dispositivo(s) — toca para seleccionar" / "%d device(s) — tap to select"
    UNNAMED,               // "(sin nombre)" / "(unnamed)"
    BRIGHTNESS_LABEL,      // "Brillo de pantalla" / "Screen brightness"
    // ── Solar charge-state strings (Victron names translated) ────────────────
    SOL_STATUS_OFF,        // "Apagado"           / "Off"
    SOL_STATUS_LOW_POWER,  // "Baja potencia"     / "Low power"
    SOL_STATUS_FAULT,      // "Aver\xC3\xAD" "a"  / "Fault"
    SOL_STATUS_BULK,       // "Carga"             / "Bulk"
    SOL_STATUS_ABSORB,     // "Absorci\xC3\xB3n"  / "Absorption"
    SOL_STATUS_FLOAT,      // "Flotaci\xC3\xB3n"  / "Float"
    // ── WebSocket reverse tunnel ───────────────────────────────────────────
    TUNNEL_CFG,            // settings menu: "T\xC3\xBAnel" / "Tunnel"
    TUNNEL_TITLE,
    TUNNEL_ENABLE,
    TUNNEL_SERVER,
    TUNNEL_SERVER_PH,
    TUNNEL_TOKEN,
    TUNNEL_TOKEN_PH,
    TUNNEL_INSTR,
    TUNNEL_STATUS_ON,
    TUNNEL_STATUS_OFF,
    _COUNT
};

const char*  t(TK key);
Language     currentLanguage();
bool         isLanguageSet();
void         setLanguage(Language lang);
void         loadLanguage();
