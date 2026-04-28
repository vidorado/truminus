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

For CYD builds `wifi_config.h` can be empty — WiFi and MQTT are configured interactively on the display at first boot.

## Architecture

### Communication Flow

```
Truma Combi D ←→ LIN transceiver ←→ ESP32 UART
                                       ↕
                              MQTT broker / Web clients / Serial
```

### Source Files (`src/`)

- **`main.cpp`** — Entry point. Owns WiFi/MQTT lifecycle, the LIN bus task (Core 0), OTA, serial CLI dispatch, and LED status task. Defines hardware pins per board variant.
- **`trumaframes.hpp/.cpp`** — Protocol layer. Defines all readable and writable LIN frames plus master frames. Each frame class parses raw bytes and publishes to MQTT/WebSocket.
- **`settings.hpp/.cpp`** — Setpoint abstraction layer. Each setting (temperature, boiler mode, fan mode, etc.) validates input from MQTT, WebSocket, or serial, then stores a value consumed by the main loop to write to the Truma via LIN.
- **`globals.hpp`** — Shared instances: `mqttClient`, `ws` (WebSocket), base MQTT topics, and Home Assistant autodiscovery identifiers.
- **`autodiscovery.hpp/.cpp`** — Builds and publishes Home Assistant MQTT discovery payloads. Enabled with `-DAUTODISCOVERY` build flag.
- **`webserver.hpp/.cpp`** — Initializes LittleFS static file serving and handles incoming WebSocket JSON messages by routing them to `settings.cpp`.
- **`waterboost.hpp/.cpp`** — Manages a 40-minute high-temperature water heating cycle triggered when boiler mode is "boost".
- **`commandreader.hpp/.cpp`** — Buffers serial input and extracts complete command lines for the CLI in `main.cpp`.
- **`cyddisplay.hpp/.cpp`** — CYD display and touch UI (only compiled with `-DCYD`).
- **`wifisetup.hpp/.cpp`** — Blocking LVGL screens for WiFi/MQTT setup and touch calibration (CYD only).

### Web Interface (`data/`)

Static files served from LittleFS. `script.js` communicates via WebSocket (JSON). Must be uploaded separately with `uploadfs` whenever changed.

## Key Design Patterns

- **Conditional compilation**: `WEBSERVER`, `AUTODISCOVERY`, `COMBIGAS` (WIP for gas-only variant), `CYD` flags gate entire features.
- **Settings flow**: External input (MQTT/WS/serial) → `settings.cpp` validates → `main.cpp` loop reads value → writes to Truma frame → Truma responds → frame published back to MQTT/WS.
- **LIN bus task**: Runs pinned to Core 0 so blocking serial reads don't interfere with WiFi/MQTT/LVGL on Core 1.
- **MQTT publish optimization**: Values are only published on change or after a 10-second timeout to avoid flooding the broker.
- **LED task**: Runs as a separate FreeRTOS task (non-CYD builds); blink count encodes connection state (1=no WiFi, 2=no MQTT, 3=LIN error, 4=reset in progress).

## MQTT Topics

Base topics are defined in `globals.hpp`:
- Status: `truma/status/<field>`
- Setpoints: `truma/set/<field>`

Writable setpoints: `temp`, `heating`, `boiler` (off/eco/high/boost), `fan` (off/eco/high/1–10), `energy_idx` (0–4), `simultemp`, `error_reset`, `refresh`, `ping`.

---

## Target Hardware: ESP32-2432S028R ("Cheap Yellow Display" / CYD)

- Model: **ESP32-2432S028R** (R = resistive touch)
- MCU: ESP32-WROOM-32, dual-core Xtensa LX6, 240 MHz
- Display: **ILI9341** 2.8" TFT, 320×240 landscape
- Touch: **XPT2046** resistive
- PlatformIO board id: `esp32-2432S028R`

### LIN bus UART pins
- **TX → GPIO 27** (CN2 connector, bottom-right)
- **RX → GPIO 22** (P3 connector, bottom-right)

These are the only GPIOs available that aren't consumed by display, touch, SD, RGB LED, or USB serial.

⚠️ P3 also exposes GPIO 21 (backlight PWM) — don't use it for sensor VCC.

### Display library
Use **esp32-smartdisplay** (`rzeldent/esp32-smartdisplay`) — handles all SPI pin init, LVGL integration, touch, and backlight for this board automatically via `board = esp32-2432S028R`.

### External temperature sensor
AM2301 (DHT22-compatible) on GPIO 17 (repurposed from RGB LED blue, LED removed). Read every 30 s, broadcast to WebSocket clients as `outdoor_temp`.

---

## CYD Display Implementation (`src/cyddisplay.cpp/.hpp`)

### Layout (320×240 landscape)
Three horizontal zones: top bar (logo, temperatures, status icons, gear button), content area (two-column: left=heating, right=hot water), status bar (logo, IP, status message). A vertical separator divides the two content columns roughly in half.

### Custom font (`src/symbols_14.c`)
FontAwesome 5 Solid subset generated by `scripts/gen_symbols.py`. Contains the tint (water drop), home/thermometer, fire, thermometer-half, and chevron-right glyphs. Regenerate with `python scripts/gen_symbols.py` (requires `pip install freetype-py`).

### `cydDisplayInit(roomSp, waterSp, heatingOn, fanMode)`
Called once from `setup()` after all settings objects exist. Shows a 2-second splash, then builds the full UI and loads persisted settings (timeout index from NVS).

### `cydDisplayUpdate(...)`
Called from `loop()` every ~10 ms under the LVGL mutex. Updates temperatures, status dots, error indicators, screen timeout/dimming, and calls `refreshControls()` to re-sync buttons to current setting values.

### Settings screens
Accessed via the gear button in the top bar. Full-screen overlays (no separate screen objects) for WiFi config, MQTT config, and display timeout. WiFi/MQTT changes save to NVS and trigger `ESP.restart()`.

### Navigation request pattern
Settings screens that need blocking setup (WiFi, MQTT) set a flag (`s_navRequest`) from the LVGL callback. `loop()` reads the flag outside the LVGL mutex, takes the mutex, runs the blocking setup screen, then restarts. This avoids re-entrant LVGL calls.

### Screen timeout
Four options (30 s, 1 min, 3 min, never), persisted to NVS. Implemented as three stages: full brightness → dim warning (5 s before timeout) → backlight off with a transparent wake overlay on `lv_layer_top()`. First touch while off wakes the screen without passing the tap to the UI beneath.

### Fan/heating interaction rules
- Toggling heat OFF forces fan to "off".
- Heat ON → shows setpoint row + heating fan row (Eco/Alto/Apag.).
- Heat OFF → shows On/Off fan row + optional level row (1–10), both disabled while boiler is active.
- Activating boiler forces fan to "off" (prevents unexpected fan starts when the boiler cycles).

### Boiler modes & energy selection
Four boiler buttons (off / 40°C / 60°C / 60°C⚡boost) map to `TBoilerSetting` values. Energy dropdown (Gas / Gas+Elec 850W / Gas+Elec 1700W / Elec 850W / Elec 1700W) maps to `TEnergySelection` (0–4); `main.cpp` polls `cydGetEnergyMode()` each cycle and applies it to `EnergySelect` and `SetPowerLimit` frames.

### LVGL task / mutex
`lv_timer_handler()` runs in a dedicated FreeRTOS task (Core 1) every 5 ms. Any LVGL call from `loop()` or `linBusTask()` must be wrapped in `lvglLock()`/`lvglUnlock()`. The `loop()` uses a 10 ms timeout instead of `portMAX_DELAY` to avoid stalling when the LVGL task is busy.

### NVS namespaces
| Namespace | Keys | Content |
|-----------|------|---------|
| `"wifi"` | `ssid`, `pass` | WiFi credentials |
| `"mqtt"` | `host`, `port`, `user`, `pass` | MQTT config |
| `"touchcal"` | `data` | Touch calibration matrix |
| `"cyd"` | `timeout_idx` | Screen timeout option index |

### wifisetup.hpp public API
```cpp
void runWifiSetup(String& ssid, String& pass);
void runMqttSetup(String& uri, String& user, String& pass);
void runTouchCalibration();
bool loadWifiCredentials(String& ssid, String& pass);
bool loadMqttConfig(String& host, String& port, String& user, String& pass);
```
Both `run*` functions save to NVS internally; credentials take effect after `ESP.restart()`.

---

### TrumaDisplay project (https://github.com/olivluca/TrumaDisplay)
An alternative MQTT client for the CYD that talks to TruMinus topics (same MQTT lib: `cyijun/ESP32MQTTClient`). UI built with Squareline Studio → LVGL. Uses a `std::queue<String>` + `std::mutex` to hand MQTT callbacks to the main thread safely. Has a heartbeat watchdog (15 s timeout on `truma/status/heartbeat`).
