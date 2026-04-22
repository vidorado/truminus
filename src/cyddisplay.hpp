#pragma once
#ifdef CYD
#include <Arduino.h>
#include "settings.hpp"

// Llamar una vez en setup(), después de inicializar el display.
// Recibe punteros a los objetos de configuración para mostrar y controlar.
void cydDisplayInit(TTempSetting*   roomSetpoint,
                    TBoilerSetting* waterSetpoint,
                    TOnOffSetting*  heatingOn,
                    TFanSetting*    fanMode);

// Llamar en loop() — actualiza indicadores de estado y controles.
// mqttEnabled: false si el usuario omitió la configuración MQTT.
// roomTemp / waterTemp: -273 si todavía no hay lectura válida del bus LIN.
void cydDisplayUpdate(bool wifiok, bool mqttok, bool trumaok,
                      bool truma_reset, bool inota,
                      bool mqttEnabled,
                      float roomTemp = -273.0f,
                      float waterTemp = -273.0f);

#endif // CYD
