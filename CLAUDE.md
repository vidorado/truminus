# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

TruMinus is an ESP32 firmware that emulates a CP-Plus control unit to manage a **Truma Combi D** heating/boiler unit via LIN bus. It exposes control through MQTT, a WebSocket-based web interface, and a serial CLI.

## Build System

This project uses **PlatformIO** (not plain Arduino IDE or CMake).

```bash
# Build and upload firmware
pio run --target upload

# Build and upload the web interface (LittleFS filesystem)
pio run --target buildfs
pio run --target uploadfs

# OTA upload (device must be on the network as truminus.local)
# Change upload_protocol in platformio.ini to 'espota' first
pio run --target upload
```

### Board Selection

Three board presets are defined in `platformio.ini`. Activate one by commenting/uncommenting the relevant `build_flags` block:

| Board | Flag | Notes |
|-------|------|-------|
| GOOUUU ESP32 C3 (default) | `-DGOOUUUC3` | RGB LED, TX=19, RX=18 |
| Wroom32 | `-DWROOM32` | Single LED, TX=19, RX=18 |
| C3 Supermini | `-DC3SUPERMINI` | Single LED (inverted) pin 8, TX=6, RX=7 |

**Important for C3 Supermini**: requires `-DARDUINO_USB_CDC_ON_BOOT=1` in build flags.

### Required User File

Before building, create `src/wifi_config.h` (not tracked in git):

```cpp
#define WLAN_SSID "your_ssid"
#define WLAN_PASS "your_password"
#define MQTT_URI "mqtt://x.x.x.x:1883"
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""
```

## Architecture

### Communication Flow

```
Truma Combi D ←→ LIN transceiver ←→ ESP32 UART
                                       ↕
                              MQTT broker / Web clients / Serial
```

### Source Files (`src/`)

- **`main.cpp`** — Entry point. Owns WiFi/MQTT lifecycle, the main loop (LIN polling every ~100ms), OTA, serial CLI dispatch, and LED status task. Defines hardware pins per board variant.
- **`trumaframes.hpp/.cpp`** — Protocol layer. Defines all readable (0x16, 0x34, 0x35, 0x39, 0x3b) and writable (0x02–0x07) LIN frames plus master frames. Each frame class parses raw bytes and publishes to MQTT.
- **`settings.hpp/.cpp`** — The setpoint abstraction layer. Each setting (temperature, boiler mode, fan mode, etc.) validates input from MQTT, WebSocket, or serial, then stores a pending value consumed by the main loop to write to the Truma via LIN.
- **`globals.hpp`** — Shared instances: `mqttClient`, `ws` (WebSocket), base MQTT topics (`BASETOPIC`, `BASESETTP`), and Home Assistant autodiscovery identifiers.
- **`autodiscovery.hpp/.cpp`** — Builds and publishes Home Assistant MQTT discovery payloads. Enabled with `-DAUTODISCOVERY` build flag.
- **`webserver.hpp/.cpp`** — Initializes LittleFS static file serving and handles incoming WebSocket JSON messages by routing them to `settings.cpp`.
- **`waterboost.hpp/.cpp`** — Manages a 40-minute high-temperature water heating cycle triggered when boiler setpoint ≥ 60°C.
- **`commandreader.hpp/.cpp`** — Buffers serial input and extracts complete command lines for the CLI in `main.cpp`.

### Web Interface (`data/`)

Static files served from LittleFS. `script.js` communicates via WebSocket (JSON). Must be uploaded separately with `uploadfs` whenever changed.

## Key Design Patterns

- **Conditional compilation**: `WEBSERVER`, `AUTODISCOVERY`, `COMBIGAS` (WIP for gas-only variant) flags gate entire features.
- **Settings flow**: External input (MQTT/WS/serial) → `settings.cpp` validates and marks pending → `main.cpp` loop reads pending value → writes to Truma frame → Truma responds → frame publish back to MQTT/WS.
- **MQTT publish optimization**: Values are only published on change or after a 10-second timeout to avoid flooding the broker.
- **LED task**: Runs as a separate FreeRTOS task; blink count encodes connection state (1=no WiFi, 2=no MQTT, 3=LIN error, 4=reset in progress).

## MQTT Topics

Base topics are defined in `globals.hpp`:
- Status: `truma/status/<field>`
- Setpoints: `truma/set/<field>`

Writable setpoints: `temp`, `heating`, `boiler` (off/eco/high/boost), `fan` (off/eco/high/1–10), `simultemp`, `reset`, `refresh`, `ping`.

---

## Target Hardware: ESP32-2432S028R ("Cheap Yellow Display" / CYD)

### Board identity
- Model: **ESP32-2432S028R** (R = resistive touch)
- MCU: **ESP32-S** (ESP32-WROOM-32 style, dual-core Xtensa LX6, 240 MHz, 520 KB SRAM, 4 MB flash)
- Manufacturer label on board: **Guition**
- PlatformIO board id: `esp32-2432S028R`
- Display: **ILI9341** 2.8" TFT, 320×240, RGB565
- Touch: **XPT2046** resistive
- Backlight: GPIO 21 (PWM)
- Light sensor (LDR): GPIO 34

### Display SPI (dedicated bus)
| Signal | GPIO |
|--------|------|
| MOSI   | 13   |
| MISO   | 12   |
| SCK    | 14   |
| CS     | 15   |
| DC     | 2    |
| RST    | –1 (software) |

### Touch SPI (separate bus)
| Signal | GPIO |
|--------|------|
| MOSI   | 32   |
| MISO   | 39   |
| SCK    | 25   |
| CS     | 33   |
| IRQ    | 36   |

### RGB LED (active LOW)
| Color | GPIO |
|-------|------|
| R     | 4    |
| G     | 16   |
| B     | 17   |

### SD card SPI
| Signal | GPIO |
|--------|------|
| MOSI   | 23   |
| MISO   | 19   |
| SCK    | 18   |
| CS     | 5    |

### Other on-board peripherals
| Peripheral | GPIO |
|------------|------|
| Speaker    | 26   |
| USB Serial TX | 1 |
| USB Serial RX | 3 |
| BOOT button | 0  |

### External connectors
- **P3** (4-pin JST bottom-right): GND, 3.3V, IO35 (input-only), IO22
- **CN2** (bottom-right): exposes additional GPIOs including IO27
- **P4** (top, 4-pin): SPI expansion / IO pins
- **SPEAK** (top, 2-pin JST): speaker output (GPIO 26)
- Left edge header (unpopulated): TX, RX, S, GND

### Available GPIOs for LIN bus UART
Pins not consumed by display/touch/LED/SD/USB:
- **TX → GPIO 27** (on CN2)
- **RX → GPIO 22** (on P3 connector)
- GPIO 35 is input-only (unusable for TX)
- GPIO 26 is speaker (could repurpose if speaker unused)

### Display library
Use **esp32-smartdisplay** (`rzeldent/esp32-smartdisplay`) — handles all pin init, LVGL integration, touch calibration, and brightness/LDR for this board automatically when `board = esp32-2432S028R`.

Touch calibration constants (hardcoded in TrumaDisplay, portrait→landscape rotation):
```cpp
lv_point_t screen[] = {{0,319},{0,0},{239,319}};
lv_point_t touch[]  = {{15,288},{17,15},{224,288}};
```

---

## CYD Display Implementation (`src/cyddisplay.cpp/.hpp`)

### Layout (320×240 landscape)
```
y=0..27    Top bar (28 px) — logo, temps, status icons, gear button
y=28       Horizontal separator
y=29..203  Content (175 px)  → CONT_H = 175, Y_CONT = 29
y=204      Horizontal separator
y=205..239 Status bar (35 px) — TruMinus logo, IP, status message

x=0..152   Left panel  CALEFACCION (W_L=153)  X_DIV=153
x=153      Vertical separator
x=154..319 Right panel AGUA CALIENTE (W_R=166) X_R=154
```

### Left panel Y positions (relative to Y_CONT=29)
| Element | Y offset | Height |
|---------|----------|--------|
| "CALEFACCION" label | +2 | — |
| Heat on/off button | +20 | 36 |
| SP row (▼ temp ▲) | +62 | 35, hidden when heat OFF |
| "VENTILADOR" label | +99 | — |
| Fan heating row (Eco/Alto/Apag.) | +115 | 28, visible heat ON |
| Fan off-mode row (On/Off) | +115 | 28, visible heat OFF |
| Fan level row (▼ N ▲) | +144 | 28, visible heat OFF + fan On |

### Right panel Y positions
| Element | Y offset | Height |
|---------|----------|--------|
| "AGUA CALIENTE" label | +2 | — |
| Boiler buttons 2×2 grid | +20, +62 | 38 each |
| "ENERGIA" label | +108 | — |
| Energy dropdown | +124 | 28 |

### Colour palette (hex)
`C_BG=0x1a1a2e`, `C_TOPBAR=0x0f0f22`, `C_SEP=0x444466`, `C_LABEL=0x8888aa`,
`C_BTN_OFF=0x2a2a4a`, `C_BTN_ON=0x3a7bd5`, `C_HEAT_ON=0x1a8a3a`, `C_TEXT=0xffffff`

### Custom font (`src/symbols_14.c`)
Generated by `scripts/gen_symbols.py` from `FontAwesome5-Solid.woff` (in `scripts/fonts/`).
- Size 14 px, 4 bpp, LINE_H=14, BASE_LINE=3
- Glyphs: U+F043 `fa-tint` (water drop, tint icon), U+F015 `fa-home` (used as thermometer)
- Used as `symbols_14` font; fallback = `lv_font_montserrat_14`
- Macros: `MY_SYMBOL_THERMOMETER` = `\xEF\x80\x95`, `MY_SYMBOL_TINT` = `\xEF\x81\x83`
- Regenerate: `python scripts/gen_symbols.py` (requires `pip install freetype-py`)
- The script auto-clips glyphs taller than LINE_H to avoid LVGL rendering artifacts

### `cydDisplayInit(roomSp, waterSp, heatingOn, fanMode)`
1. Loads display-timeout index from NVS (namespace `"cyd"`, key `"timeout_idx"`)
2. Shows 2-second splash (logo centred, dark background) — reuses same buffer
3. Builds full UI (top bar, separators, left/right panels, status bar)
4. Calls `refreshControls()` + places logo in status bar

### `cydDisplayUpdate(wifiok, mqttok, trumaok, truma_reset, inota, mqttEnabled, roomTemp, waterTemp, waterHeating)`
Called from `main.cpp` loop() every ~10 ms under the LVGL mutex.
- Checks screen timeout (`lv_display_get_inactive_time`) → backlight off + wake overlay on `lv_layer_top()`
- Updates temperature labels, Wi-Fi/MQTT/LIN status dots, IP, status message
- 1 Hz blink on tint icon when `waterHeating` is true
- Calls `refreshControls()` (re-syncs buttons to setting values, handles fan/SP visibility)

### Settings screens (all inside cyddisplay.cpp, no external deps beyond wifisetup.hpp)
**Gear button** (top bar, x=215): opens `showSettingsMenu()`

**`showSettingsMenu()`** — full-screen dark overlay with back + 3 buttons:
- **WiFi Config** → sets `s_navRequest = WifiSetup`, loads s_scr, deletes self
- **MQTT Config** → sets `s_navRequest = MqttSetup`, loads s_scr, deletes self
- **Pantalla** → calls `showDisplaySettings()`, deletes self

**`showDisplaySettings()`** — 3 option buttons (radio style):
- Options: "1 minuto" (60 s), "3 minutos" (180 s), "No apagar" (0 = disabled)
- Tap → highlight + save to NVS immediately (`saveTimeoutIdx()`)
- Back → `showSettingsMenu()`

### Navigation request pattern (thread-safe)
```cpp
// cyddisplay.cpp sets flag from LVGL callback (lvglTask, Core 1):
s_navRequest = CydNavRequest::WifiSetup;

// main.cpp loop() reads flag OUTSIDE mutex, drives blocking setup screen:
CydNavRequest nav = cydGetNavRequest();
if (nav != CydNavRequest::None) {
    cydClearNavRequest();
    lvglLock();                    // blocks lvglTask
    if (nav == CydNavRequest::WifiSetup) { String ss,pp; runWifiSetup(ss,pp); }
    else                                 { String u,us,pp; runMqttSetup(u,us,pp); }
    cydReloadScreen();             // restore s_scr + backlight
    lvglUnlock();
    ESP.restart();                 // apply new NVS credentials
}
```

### Screen timeout / wake on touch
- `TIMEOUT_MS[] = {30000, 60000, 180000, 0}` — default index 0 (30 s)
- **Three stages**: full brightness → 50% dim warning (5 s before) → backlight off
- On timeout: `smartdisplay_lcd_set_backlight(0.0f)` + transparent overlay on `lv_layer_top()`
- On first touch while off: `wakeCb` turns backlight back on (1.0f), deletes overlay
- On touch while dimmed: LVGL resets `lv_display_get_inactive_time` → next cydDisplayUpdate restores 1.0f automatically (no extra code needed)
- `s_screenDimmed` flag prevents repeated set_backlight calls every 10 ms
- Backlight GPIO confirmed: **GPIO 21** (`GPIO_BCKL=21` in `esp32-2432S028R.json`)
  - ⚠️ P3 connector exposes GPIO21 — conflicts with backlight PWM if used for sensor VCC
- Index persisted in NVS namespace `"cyd"`, key `"timeout_idx"`
- `cydReloadScreen()`: cleans overlay + forces backlight on + loads s_scr

### Fan/heating interaction
- `heatCb`: toggling heat OFF forces fan to `"off"` via `s_fanMode->setValue("off")`
- Heating ON → shows SP row + fanHeatingRow (Eco/Alto/Apag.)
- Heating OFF → hides SP row, shows fanOffRow (On/Off) + optional fanLevelRow (1-10)

### Boiler modes & energy selection
- Boiler buttons: `BOILER_STR[] = {"off","eco","high","boost"}` → labels "Apag./40°C/60°C/60°C⚡"
- Energy dropdown: Gas / Gas+Elec.850W / Gas+Elec.1700W / Elec.850W / Elec.1700W
  → maps to `TEnergySelection` enum (EsGasDiesel=0..EsElectro1800=4)
- `cydGetEnergyMode()` returns current index; main.cpp calls:
  ```cpp
  auto em = (TEnergySelection)cydGetEnergyMode();
  EnergySelect->setEnergySelection(em);
  SetPowerLimit->setPowerLimit(em);
  ```

### LVGL task / mutex (main.cpp)
```cpp
// Core 1 task — holds mutex while calling lv_timer_handler() every 5 ms
static SemaphoreHandle_t s_lvglMutex;
static inline void lvglLock()   { xSemaphoreTake(s_lvglMutex, portMAX_DELAY); }
static inline void lvglUnlock() { xSemaphoreGive(s_lvglMutex); }
// main loop uses pdMS_TO_TICKS(10) timeout to avoid blocking
```

### Backlight control
```cpp
smartdisplay_lcd_set_backlight(float duty);  // 0.0–1.0
// GPIO 21, PWM 400 Hz 8-bit via esp32_smartdisplay
```

### NVS namespaces used by CYD code
| Namespace | Key | Content |
|-----------|-----|---------|
| `"wifi"` | `"ssid"`, `"pass"` | WiFi credentials (wifisetup.cpp) |
| `"mqtt"` | `"host"`, `"port"`, `"user"`, `"pass"` | MQTT config (wifisetup.cpp) |
| `"touchcal"` | `"data"` | Touch calibration matrix (wifisetup.cpp) |
| `"cyd"` | `"timeout_idx"` | Screen timeout option index 0-2 (cyddisplay.cpp) |

### wifisetup.hpp public API
```cpp
void runWifiSetup(String& ssid, String& pass);     // blocking LVGL screen
void runMqttSetup(String& uri, String& user, String& pass); // blocking LVGL screen
void runTouchCalibration();
bool loadWifiCredentials(String& ssid, String& pass);
bool loadMqttConfig(String& host, String& port, String& user, String& pass);
```
Both `run*` functions save to NVS internally; credentials take effect after `ESP.restart()`.

---

### TrumaDisplay project (https://github.com/olivluca/TrumaDisplay)
MQTT client for the CYD that talks to TruMinus topics. Key details for the merge:
- Same MQTT lib: `cyijun/ESP32MQTTClient`
- Subscribes to `truma/#`; publishes to `truma/set/*`
- UI built with **Squareline Studio** → LVGL (`ui/ui.h`)
- LVGL widgets: `ui_Temp`, `ui_Heating` (checkbox), `ui_Boiler` (dropdown), `ui_Fan` (dropdown), `ui_RoomTemp`, `ui_WaterTemp`, `ui_Voltage`, `ui_Window`, `ui_RoomDemand`, `ui_WaterDemand`, `ui_Waterboost`, `ui_ErrClass`, `ui_ErrCode`, `ui_ResetButton`, `ui_ScreenOff`, `ui_TrumaMainScreen`, `ui_ErrorScreen`, `ui_ErrorLabel`, `ui_Keyboard`
- Uses a `std::queue<String>` + `std::mutex` to safely hand MQTT callbacks to the main thread
- LVGL timer: `lv_timer_handler()` called every loop iteration with `lv_tick_inc(delta_ms)`
- Heartbeat watchdog: if no `truma/status/heartbeat` for 15 s → shows error screen
