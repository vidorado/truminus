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

### TrumaDisplay project (https://github.com/olivluca/TrumaDisplay)
MQTT client for the CYD that talks to TruMinus topics. Key details for the merge:
- Same MQTT lib: `cyijun/ESP32MQTTClient`
- Subscribes to `truma/#`; publishes to `truma/set/*`
- UI built with **Squareline Studio** → LVGL (`ui/ui.h`)
- LVGL widgets: `ui_Temp`, `ui_Heating` (checkbox), `ui_Boiler` (dropdown), `ui_Fan` (dropdown), `ui_RoomTemp`, `ui_WaterTemp`, `ui_Voltage`, `ui_Window`, `ui_RoomDemand`, `ui_WaterDemand`, `ui_Waterboost`, `ui_ErrClass`, `ui_ErrCode`, `ui_ResetButton`, `ui_ScreenOff`, `ui_TrumaMainScreen`, `ui_ErrorScreen`, `ui_ErrorLabel`, `ui_Keyboard`
- Uses a `std::queue<String>` + `std::mutex` to safely hand MQTT callbacks to the main thread
- LVGL timer: `lv_timer_handler()` called every loop iteration with `lv_tick_inc(delta_ms)`
- Heartbeat watchdog: if no `truma/status/heartbeat` for 15 s → shows error screen
