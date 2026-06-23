#pragma once
#include "openairble.hpp"
#include "p4display.hpp"

// Cached "OpenAir PLUS A/C configured" flag (NVS namespace "openair", key "addr").
// Drives the CALEFACCIÓN→CLIMATIZACIÓN panel switch on both UIs.
//
// IMPORTANT: never read NVS on every main-loop tick.  An NVS/flash read takes
// the flash lock and briefly disables the cache; the MIPI-DSI ISR is not
// cache-safe (CONFIG_LCD_DSI_ISR_CACHE_SAFE off), so a periodic read starves it
// and the panel shows a full-frame underrun glitch.  Read once at boot and only
// re-read when the A/C config is saved (openairCfgReload(), called from the
// settings screen) so the panel switches without a reboot.

void openairCfgReload();          // re-reads NVS; call at boot + after A/C settings saved
bool openairCfgIsActive();        // cached flag — never call NVS inline

// Translate the current P4ControlState A/C fields into an OpenAir PLUS BLE command.
// Called whenever the control state may have changed so the next BLE poll
// delivers the latest setpoint + mode.
//
// acMode mapping:  0=off → PowerState=0
//                  1=cool → PowerState=1, Mode=AUTO or MAN (per acFanAuto)
//                  2=eco  → PowerState=1, Mode=1 (ECO — TO VERIFY on real unit)
// Setpoint clamped to 16.0–32.0 °C (gauge minimum confirmed 16.0 from the app).
OpenAirCmd buildOpenAirCmd(const P4ControlState& cs);
