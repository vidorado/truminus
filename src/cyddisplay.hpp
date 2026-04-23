#pragma once
#ifdef CYD
#include <Arduino.h>
#include "settings.hpp"

// Llamar una vez en setup(), después de inicializar el display.
void cydDisplayInit(TTempSetting*   roomSetpoint,
                    TBoilerSetting* waterSetpoint,
                    TOnOffSetting*  heatingOn,
                    TFanSetting*    fanMode);

// Llamar en loop() — actualiza indicadores de estado y controles.
void cydDisplayUpdate(bool wifiok, bool mqttok, bool trumaok,
                      bool truma_reset, bool inota,
                      bool mqttEnabled,
                      float roomTemp     = -273.0f,
                      float waterTemp    = -273.0f,
                      bool  waterHeating = false,
                      float extTemp      = -273.0f);   // temperatura exterior AM2301

// Índice del modo de energía seleccionado en pantalla (0-4).
// Mapea a TEnergySelection: 0=EsGasDiesel, 1=EsMixed900, 2=EsMixed800,
//                           3=EsElectro900, 4=EsElectro1800
int  cydGetEnergyMode();
// Sincroniza el dropdown con un índice recibido de WS/MQTT (llamar bajo mutex LVGL).
void cydSetEnergyIdx(int idx);

// ── Configuración / navegación ─────────────────────────────────────────────
// Los botones de la pantalla de ajustes fijan este flag desde callbacks de LVGL.
// main.cpp lo lee en loop() FUERA del mutex para poder llamar a las pantallas
// bloqueantes runWifiSetup / runMqttSetup de forma segura.
enum class CydNavRequest { None, WifiSetup, MqttSetup };
CydNavRequest cydGetNavRequest();
void          cydClearNavRequest();
// Recargar s_scr (llamar tras volver de una pantalla de configuración).
// También enciende el backlight y limpia cualquier overlay de apagado.
void          cydReloadScreen();

#endif // CYD
