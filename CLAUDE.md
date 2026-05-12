# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

TruMinus is an ESP32 firmware that emulates a CP-Plus control unit to manage a **Truma Combi D** heating/boiler unit via LIN bus. It exposes control through MQTT, a WebSocket-based web interface, a serial CLI, and (on CYD hardware) a physical touchscreen.

**New:** Solar charge (Victron BLE) and battery SOC (Ultimatron BLE) panels are now displayed on both the CYD physical screen and the web interface. BLE is disabled by default on CYD builds to prevent OOM crashes during concurrent HTTP requests.

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

### Web assets are embedded, not served from LittleFS

Files in `data/` are compressed by `scripts/compress_fs.py` into `src/webfiles.h` as `static const uint8_t` arrays. **Firmware reflash is required** after any `data/` change. `uploadfs` alone is insufficient.

### Board Selection

Board presets are defined in `platformio.ini`. Activate one by selecting the corresponding `[env:…]` section:

| Board | Flag | Notes |
|-------|------|-------|
| GOOUUU ESP32 C3 (default) | `-DGOOUUUC3` | RGB LED, TX=19, RX=18 |
| Wroom32 | `-DWROOM32` | Single LED, TX=19, RX=18 |
| C3 Supermini | `-DC3SUPERMINI` | Single LED (inverted) pin 8, TX=6, RX=7 |
| **CYD** | **`-DCYD`** | ESP32-2432S028R, 320×240 TFT, resistive touch, solar/battery UI |
| **CYD_C5** | **`-DCYD_C5`** | ESP32-C5-WROOM-1 (NM-CYD-C5), 320×240 ST7789 TFT, XPT2046 resistive touch, LIN TX=5/RX=4 (P5), DHT=27. Uses `pioarduino` platform (Arduino 3.3.6). |

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
                              MQTT broker / Web clients / Serial / CYD touch
                                       ↕
                              Victron BLE (solar) / Ultimatron BLE (battery)
```

### Source Files (`src/`)

- **`main.cpp`** — Entry point. Owns WiFi/MQTT lifecycle, the LIN bus task (Core 0), OTA, serial CLI dispatch, LED status task, and solar/battery data broadcast. Defines hardware pins per board variant.
- **`trumaframes.hpp/.cpp`** — Protocol layer. Defines all readable and writable LIN frames plus master frames. Each frame class parses raw bytes and publishes to MQTT/WebSocket.
- **`settings.hpp/.cpp`** — Setpoint abstraction layer. Each setting (temperature, boiler mode, fan mode, etc.) validates input from MQTT, WebSocket, or serial, then stores a value consumed by the main loop to write to the Truma via LIN.
- **`globals.hpp`** — Shared instances: `mqttClient`, `ws` (WebSocket), base MQTT topics, and Home Assistant autodiscovery identifiers.
- **`autodiscovery.hpp/.cpp`** — Builds and publishes Home Assistant MQTT discovery payloads. Enabled with `-DAUTODISCOVERY` build flag.
- **`webserver.hpp/.cpp`** — Initializes static file serving (from embedded flash) and handles incoming WebSocket JSON messages by routing them to `settings.cpp`.
- **`waterboost.hpp/.cpp`** — Manages a 40-minute high-temperature water heating cycle triggered when boiler mode is "boost".
- **`commandreader.hpp/.cpp`** — Buffers serial input and extracts complete command lines for the CLI in `main.cpp`.
- **`cyddisplay.hpp/.cpp`** — CYD display and touch UI (only compiled with `-DCYD`). Full 4-panel layout with solar/battery.
- **`wifisetup.hpp/.cpp`** — Blocking LVGL screens for WiFi/MQTT setup, touch calibration, **solar config** (Victron MAC + key), and **battery config** (Ultimatron MAC) (CYD only).
- **`victronble.hpp/.cpp`** — Victron Solar Charger BLE listener (Instant Readout protocol). Disabled when `-DBLE` is not set; provides simulated data stubs instead.
- **`ultimatronble.hpp/.cpp`** — Ultimatron LiFePO4 BMS BLE listener (GATT protocol). Disabled when `-DBLE` is not set; provides simulated data stubs instead.
- **`i18n.hpp/.cpp`** — Internationalization. `TK` enum with all UI strings in Spanish and English. `t(TK::KEY)` returns the current language string.

### Web Interface (`data/`)

Static files are compressed into `src/webfiles.h` and served from embedded flash. `script.js` communicates via WebSocket (JSON). Must be followed by **firmware reflash** (`pio run --target upload`) whenever changed.

## Key Design Patterns

- **Conditional compilation**: `WEBSERVER`, `AUTODISCOVERY`, `COMBIGAS` (WIP for gas-only variant), `CYD`, `BLE` flags gate entire features.
- **Settings flow**: External input (MQTT/WS/serial/CYD touch) → `settings.cpp` validates → `main.cpp` loop reads value → writes to Truma frame → Truma responds → frame published back to MQTT/WS/CYD.
- **LIN bus task**: Runs pinned to Core 0 so blocking serial reads don't interfere with WiFi/MQTT/LVGL on Core 1.
- **MQTT publish optimization**: Values are only published on change or after a 10-second timeout to avoid flooding the broker.
- **LED task**: Runs as a separate FreeRTOS task (non-CYD builds); blink count encodes connection state (1=no WiFi, 2=no MQTT, 3=LIN error, 4=reset in progress).
- **BLE disabled on CYD**: The NimBLE stack consumes ~60 KB heap. Concurrent HTTP requests (e.g., web page reload) exhaust the remaining ~130 KB on ESP32-WROOM-32, causing `abort()` OOM crashes. CYD builds use simulated data stubs (oscillating `sinf()` values) so the solar/battery UI remains testable without real hardware.

## MQTT Topics

Base topics are defined in `globals.hpp`:
- Status: `truma/status/<field>`
- Setpoints: `truma/set/<field>`

Writable setpoints: `temp`, `heating`, `boiler` (off/eco/high/boost), `fan` (off/eco/high/1–10), `energy_idx` (0–4), `simultemp`, `error_reset`, `refresh`, `ping`.

---

## Target Hardware

### CYD — ESP32-2432S028R ("Cheap Yellow Display")

- Model: **ESP32-2432S028R** (R = resistive touch)
- MCU: ESP32-WROOM-32, dual-core Xtensa LX6, 240 MHz
- Display: **ILI9341** 2.8" TFT, 320×240 landscape
- Touch: **XPT2046** resistive
- PlatformIO board id: `esp32-2432S028R`

### CYD_C5 — NM-CYD-C5 (RockBase)

- MCU: **ESP32-C5-WROOM-1-N16R8**, RISC-V, 240 MHz, Wi-Fi 6 (2.4/5GHz) + BLE 5 + Zigbee/Thread
- Display: **ST7789** 2.8" TFT, 320×240 landscape
- Touch: **XPT2046** resistive (shares SPI bus with display and SD card)
- Memory: **16MB Flash / 8MB PSRAM**
- PlatformIO board id: `cyd_c5` (custom board JSON in `boards/`)
- Uses `pioarduino` platform (Arduino 3.3.6) — see `[env:cyd_c5]` in `platformio.ini`
- Onboard: RGB LED (WS2812B, IO27), SD card slot, speaker (IO26)
- Connectors: USB-C UART (CH340C), USB-C native (IO13/IO14), P5 (LP-UART IO4/IO5), CN1 (I2C IO8/IO9), FPC2 (12-pin)

### LIN bus UART pins & external temperature sensor
Pin assignments for display boards are defined conditionally in `src/main.cpp` (per board variant) and summarised in the `[env:cyd]` / `[env:cyd_c5]` sections of `platformio.ini`. Refer to those files for the definitive mapping instead of duplicating numbers here.

> ⚠️ P3 also exposes GPIO 21 (backlight PWM) — don't use it for sensor VCC.

### Display library
Use **esp32-smartdisplay** (`rzeldent/esp32-smartdisplay`) — handles all SPI pin init, LVGL integration, touch, and backlight automatically via the board definition.

### External temperature sensor
AM2301 (DHT22-compatible). Read every 30 s, broadcast to WebSocket clients as `outdoor_temp`.

---

## CYD Display Implementation (`src/cyddisplay.cpp/.hpp`)

### Layout (320×240 landscape)
Four horizontal zones: top bar, content area with **4 panels**, status bar.

**Top bar** (y=0..27): Truma logo left, outdoor temp (x=119), `⚙ Conf.` button (x=186, w=72), WiFi dot, LIN dot.

**Content area** (y=29..203, 175 px tall): divided by vertical separator at **x=141**.
- **Left panel** (141 px wide): HEATING section + FAN section
- **Right panel** (178 px wide): HOT WATER section (top) + SOLAR/BATTERY section (bottom)

**Solar/Battery section** (right panel, y=88..173):
- Left column (126 px): 4 solar data lines — Estado, Volt., Carga, Prod.
- Vertical separator (1 px)
- Right column (46 px): Battery SOC % label + vertical battery icon with fill

**Status bar** (y=205..239): Logo left, `SSID  IP` (montserrat_12, long dot mode), status message right.

### Fonts used on CYD
| Font | Size | Usage |
|------|------|-------|
| `symbols_14` | 14 px | Icons (tint, fire, thermometer, chevron) |
| `montserrat_12` | 12 px | Status bar IP/SSID, solar Volt./Carga/Prod. lines |
| `montserrat_14` | 14 px | Section labels, buttons, SOC label, most UI text |
| `montserrat_16` | 16 px | Temperature setpoint display |
| `montserrat_20` | 20 px | Titles (settings screens) |
| `montserrat_28` | 28 px | Splash screen status |

### Custom font (`src/symbols_14.c`)
FontAwesome 5 Solid subset generated by `scripts/gen_symbols.py`. Contains the tint (water drop), home/thermometer, fire, thermometer-half, and chevron-right glyphs. Regenerate with `python scripts/gen_symbols.py` (requires `pip install freetype-py`).

### `cydDisplayInit(roomSp, waterSp, heatingOn, fanMode)`
Called once from `setup()` after all settings objects exist. Shows a 2-second splash, then builds the full UI and loads persisted settings (timeout index from NVS).

### `cydDisplayUpdate(...)`
Called from `loop()` every ~10 ms under the LVGL mutex. Updates temperatures, status dots, error indicators, screen timeout/dimming, solar/battery data, and calls `refreshControls()` to re-sync buttons to current setting values.

### Settings screens
Accessed via the `⚙ Conf.` button in the top bar. Menu options:
- **WiFi Config** — blocking scan + connect; saves to NVS and reboots.
- **MQTT Config** — blocking host/port/user/pass; saves to NVS and reboots.
- **Carga solar** — Victron MAC (12 hex) + encryption key (32 hex) + Ultimatron battery MAC (12 hex). **Works without BLE** — allows manual entry for when BLE is enabled later. Saves to NVS.
- **Pantalla** — timeout selector (30 s / 1 min / 3 min / nunca). Saves to NVS immediately.
- **Idioma** — Spanish / English selector. Saves to NVS and applies immediately.

### Navigation request pattern
Settings screens that need blocking setup set a flag (`s_navRequest`) from the LVGL callback. `loop()` reads the flag outside the LVGL mutex, takes the mutex, runs the blocking setup screen, then restarts. This avoids re-entrant LVGL calls.

### Screen timeout
Four options (30 s, 1 min, 3 min, never), persisted to NVS. Implemented as three stages: full brightness → dim warning (5 s before timeout) → backlight off with a transparent wake overlay on `lv_layer_top()`. First touch while off wakes the screen without passing the tap to the UI beneath.

### Fan/heating interaction rules
- Toggling heat OFF forces fan to "off".
- Heat ON → shows setpoint row + heating fan row (Eco/Alto/Apag.).
- Heat OFF → shows On/Off fan row + optional level row (1–10), both disabled while boiler is active.
- Activating boiler forces fan to "off" (prevents unexpected fan starts when the boiler cycles).

### Boiler modes & energy selection
Four boiler buttons (off / 40°C / 60°C / 60°C⚡boost) map to `TBoilerSetting` values. Energy dropdown (Gas / Gas+Elec 850W / Gas+Elec 1700W / Elec 850W / Elec 1700W) maps to `TEnergySelection` (0–4); `main.cpp` polls `cydGetEnergyMode()` each cycle and applies it to `EnergySelect` and `SetPowerLimit` frames.

### Solar/Battery data
- `cydUpdateSolar(const CydSolarData& d)` refreshes the 4 solar lines under the LVGL mutex.
- `cydUpdateBatt(const CydBattData& d)` refreshes the SOC label and battery fill height/color.
- Data is published to web clients via WebSocket every 10 s in `publishSolarBatt()`.

### LVGL task / mutex
`lv_timer_handler()` runs in a dedicated FreeRTOS task (Core 1) every 5 ms. Any LVGL call from `loop()` or `linBusTask()` must be wrapped in `lvglLock()`/`lvglUnlock()`. The `loop()` uses a 10 ms timeout instead of `portMAX_DELAY` to avoid stalling when the LVGL task is busy.

### NVS namespaces
| Namespace | Keys | Content |
|-----------|------|---------|
| `"wifi"` | `ssid`, `pass` | WiFi credentials |
| `"mqtt"` | `host`, `port`, `user`, `pass` | MQTT config |
| `"touchcal"` | `data` | Touch calibration matrix |
| `"cyd"` | `timeout_idx`, `lang` | Screen timeout option index, language (0=ES, 1=EN) |
| `"solar"` | `addr`, `key` | Victron BLE MAC + encryption key |
| `"batt"` | `addr` | Ultimatron BLE MAC |

### wifisetup.hpp public API
```cpp
void runWifiSetup(String& ssid, String& pass);
void runMqttSetup(String& uri, String& user, String& pass);
bool runSolarSetup(String& addr, String& key);    // solar + battery config screen
void runTouchCalibration();
bool loadWifiCredentials(String& ssid, String& pass);
bool loadMqttConfig(String& host, String& port, String& user, String& pass);
bool loadSolarConfig(String& addr, String& key);
void saveSolarConfig(const String& addr, const String& key);
bool loadBattConfig(String& addr);
void saveBattConfig(const String& addr);
```
All `run*` and `save*` functions save to NVS internally; credentials take effect after `ESP.restart()`.

---

### TrumaDisplay project (https://github.com/olivluca/TrumaDisplay)
An alternative MQTT client for the CYD that talks to TruMinus topics (same MQTT lib: `cyijun/ESP32MQTTClient`). UI built with Squareline Studio → LVGL. Uses a `std::queue<String>` + `std::mutex` to hand MQTT callbacks to the main thread safely. Has a heartbeat watchdog (15 s timeout on `truma/status/heartbeat`).
