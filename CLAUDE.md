# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Language policy

**All project code, comments, identifiers, commit messages, log strings, documentation files (READMEs, SKILL.md, design notes) and PR descriptions MUST be in English.** Conversation with the user can be in Spanish, but anything that lands in the repository is English-only. UI-facing strings remain bilingual through `i18n.cpp` (`TK` enum) — never hardcode Spanish text in source files; add a key and use `t(TK::KEY)`.

## Project Overview

TruMinus is firmware for the **JC4880P443C** board (ESP32-P4) that emulates a Truma CP-Plus D control unit to manage a **Truma Combi D** heater over the LIN bus. Control surfaces: MQTT, WebSocket web UI, serial CLI, and a physical 800×480 LCD with capacitive touch.

Solar charge data (Victron BLE) and battery SOC (Ultimatron BLE) are surfaced both on the LCD and the web UI.

> **Status (2026-05):** the project is mid-migration from the previous ESP32-C5 / NM-CYD-C5 board to the JC4880P443C / ESP32-P4 board. `main/main.cpp` is currently a display-only stub; the LIN/MQTT/WiFi/BLE subsystems exist as source files (`lin_driver.cpp`, `victronble.cpp`, `ultimatronble.cpp`, `webserver.cpp`, …) but are not all wired into `app_main` yet. Treat the codebase as porting-in-progress, not feature-complete.

## Build System

The build is driven by **`idf.py`** (ESP-IDF 6.0.1) via a thin `Makefile`. PlatformIO is not used.

```bash
# Once per machine
git clone --branch release/v6.0 https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf && ./install.sh esp32p4

# Once per terminal session
. ~/esp/esp-idf/export.sh

make               # build
make flash         # build + flash (PORT=/dev/ttyACM0 by default)
make monitor       # serial monitor
make flash-monitor # flash then monitor
make clean         # idf.py fullclean
```

VSCode: install `espressif.esp-idf-extension`, copy `.vscode/settings.json.template` →
`.vscode/settings.json` and fill in your IDF path. IntelliSense reads
`build/compile_commands.json` (generated after first build).

### Critical: IDF 6.0 + ESP32-P4 build pitfalls

Before touching the build (sdkconfig, components, link errors, IRAM overflow), **read `.claude/skills/pio-idf-p4/SKILL.md`**. Key facts:

- If cmake fails with `cannot read spec file …/build/specs/picolibc.specs` or `build.ninja` is missing: **`rm -rf build/ && make build`** — the build directory is corrupted and cannot be recovered incrementally.
- ESP32-P4 rev < v3 (`chip_variant: "esp32p4_es"`) has non-contiguous SRAM (179 KB `sram_low` + 256 KB `sram_high`); `--enable-non-contiguous-regions` silently drops sections that don't fit. The cause is always an IDF config inflating IRAM, never the linker.
- ModemManager grabs `/dev/ttyACM0` on plug-in — `sudo systemctl stop ModemManager` if flash fails with "port is busy".

### Web assets are embedded, not served from LittleFS

Files in `data/` are compressed by `scripts/compress_fs.py` into `main/webfiles.h` as `static const uint8_t` arrays. **A firmware reflash is required** after any change in `data/`. There is no LittleFS partition. The root `CMakeLists.txt` re-runs `apply_patches.py`, `cache_bust.py` and `compress_fs.py` at every cmake configure so `webfiles.h` is always fresh before build.

## Architecture

### Communication Flow

```
Truma Combi D ←→ LIN transceiver ←→ ESP32-P4 UART
                                       ↕
                              MQTT broker / Web clients / Serial / Touch UI
                                       ↕
                              Victron BLE (solar) / Ultimatron BLE (battery)
```

### Source layout (`main/`)

The project follows the ESP-IDF native convention: there is no `src/`; all firmware sources live in `main/` (declared via `platformio.ini` `src_dir = main`).

- **`main.cpp`** — `app_main` entry point. Currently a stub that initialises the LCD and runs a demo update loop; the WiFi/MQTT/LIN/BLE wiring from the previous board is not all re-attached yet.
- **`p4display.cpp/.hpp`** — LVGL UI for the 800×480 LCD (replaces the old `cyddisplay.cpp`). Public surface: `p4DisplayInit()`, `p4DisplayUpdate(const P4DisplayData&)`, `p4DisplaySetStatus(const char*)`, plus `lvglLock()` / `lvglUnlock()` for callers that need to touch LVGL from other tasks.
- **`trumaframes.cpp/.hpp`** — LIN protocol layer. Each readable/writable frame class parses raw bytes and publishes to MQTT/WebSocket. See `.claude/skills/truma-protocol/SKILL.md` for the byte-level reference.
- **`lin_driver.cpp/.hpp`** — Low-level half-duplex LIN driver over ESP-IDF `driver/uart.h` (UART_NUM_1 recommended; UART_NUM_0 is the console).
- **`settings.cpp/.hpp`** — Setpoint abstraction. `TBoilerSetting`, `TTempSetting`, `TFanSetting`, `TOnOffSetting`, all derived from `TMqttSetting / TAutoDiscovery`. Single source of truth for values consumed by `main.cpp`'s LIN write loop and broadcast back to MQTT/WS/LCD.
- **`globals.hpp`** — shared `mqttClient`, `ws`, MQTT base topics, Home Assistant autodiscovery identifiers.
- **`autodiscovery.cpp/.hpp`** — Home Assistant MQTT discovery payloads. Enabled via `-DAUTODISCOVERY`.
- **`webserver.cpp/.hpp`** — HTTP static serving (from embedded flash via `webfiles.h`) plus WebSocket JSON handler that dispatches to `settings.cpp`.
- **`waterboost.cpp/.hpp`** — 40-minute high-temperature water boost cycle when boiler mode is "boost".
- **`commandreader.cpp/.hpp`** — Serial CLI line buffer for `main.cpp`.
- **`victronble.cpp/.hpp`** — Victron Solar Charger BLE listener (Instant Readout). Uses NimBLE 2.x; stubbed when `-DENABLE_BLE` is absent. See `.claude/skills/victronble/SKILL.md`.
- **`ultimatronble.cpp/.hpp`** — Ultimatron LiFePO4 BMS BLE listener (GATT). See `.claude/skills/ultimatronble/SKILL.md`.
- **`i18n.cpp/.hpp`** — `TK` enum + `t(TK::KEY)`. Spanish (default) / English; language persisted in NVS.
- **`logs.hpp`** — Logging macros / tag conventions.

### Web Interface (`data/`)

Static assets compressed into `main/webfiles.h`. `script.js` communicates with the firmware over a WebSocket using JSON envelopes (`{"command": "...", "id": "...", "value": "..."}`). **Firmware reflash is required** for every change.

## Key Design Patterns

- **Conditional compilation:** `WEBSERVER`, `AUTODISCOVERY`, `NO_MQTT`, `JC4880_P4`, `ENABLE_BLE`, `ENABLE_SOLAR_DUMMY`, `ENABLE_BOILER_DUMMY` flags gate whole features. `WEBSERVER`, `NO_MQTT` and `JC4880_P4` are pinned in the root `CMakeLists.txt` via `idf_build_set_property(COMPILE_DEFINITIONS … APPEND)` so they're visible to every component.
- **Settings flow:** external input (MQTT / WebSocket / serial CLI / touch) → `settings.cpp` validates → `main.cpp` loop reads value → writes to the right Truma LIN frame → Truma responds → frame published back to MQTT/WS/LCD.
- **LIN bus task:** the LIN UART task is pinned to **Core 0** so blocking serial reads don't fight with WiFi/MQTT/LVGL on Core 1. ESP32-P4 is dual-core (RV32IMAFC), so the pinning model from the prior C5 port still applies.
- **MQTT publish throttling:** values are published on change or after a 10 s timeout to avoid flooding the broker.
- **LVGL locking:** `lv_timer_handler()` runs on a dedicated FreeRTOS task. Any LVGL access from `app_main` / `loop` / `lin_task` must be wrapped in `lvglLock()` / `lvglUnlock()`. Prefer a short timeout (e.g. 10 ms) over `portMAX_DELAY` to avoid deadlocks if the LVGL task is busy.

## MQTT Topics

Base topics defined in `globals.hpp`:
- Status: `truma/status/<field>`
- Setpoints: `truma/set/<field>`

Writable setpoints: `temp`, `heating`, `boiler` (off/eco/high/boost), `fan` (off/eco/high/1–10), `energy_idx` (0–4), `simultemp`, `error_reset`, `refresh`, `ping`.

---

## Target Hardware — JC4880P443C (ESP32-P4)

- **MCU:** ESP32-P4-WROOM (RISC-V dual-core RV32IMAFC, 400 MHz). Silicon revision matters: the EVB module shipped here is rev < v3 (`chip_variant: "esp32p4_es"` in `boards/jc4880_p4.json`), which enables `--enable-non-contiguous-regions` on the linker. See `.claude/skills/pio-idf-p4/SKILL.md` §5.
- **Memory:** 16 MB Flash (QIO @ 80 MHz) / 32 MB PSRAM (HEX @ 200 MHz).
- **Display:** 4.3" **ST7701** RGB panel, **800×480** landscape, driven through ESP-IDF's `esp_lcd_st7701` component.
- **Touch:** **GT911** capacitive controller on the shared I2C bus (`BSP_I2C_SCL=GPIO8`, `BSP_I2C_SDA=GPIO7`). Reset and INT are not wired to dedicated pins on this board (`BSP_LCD_TOUCH_RST = GPIO_NUM_NC`).
- **Audio:** I2S codec wired (SCLK=12, MCLK=13, LCLK=10, DOUT=9, DSIN=48, PA enable=11). Not used by TruMinus yet.
- **Connectivity:** WiFi (via separate ESP32-C6 co-processor over SDIO/SPI per the BSP; see `components/jc4880_bsp/WIFI_ARCHITECTURE.md`), BLE 5 (also via the C6).
- **BSP:** vendored locally at `components/jc4880_bsp/` (forked from `csvke/esp32_p4_jc4880p433c_bsp`). Pulls `esp_lcd_st7701`, `esp_lcd_touch_gt911`, `esp_lcd_touch`, `esp_lvgl_port`, `lvgl` from the Espressif component registry on first build.
- **Upload:** USB-CDC on `/dev/ttyACM0` (no CH340/CP210x bridge needed; the P4 exposes USB natively). Speed 460800.

### Pin assignments (LIN / external sensor)

LIN UART pins and the AM2301/DHT22 external temperature sensor pin live in `main/main.cpp` and are still being finalised on the new board. **Always grep `main/main.cpp` and `main/lin_driver.cpp` for the current mapping rather than relying on this document.** The previous C5 board used TX=GPIO5 / RX=GPIO4 (P5 LP-UART), DHT=GPIO27 — those pin numbers do NOT apply on the JC4880-P4 because GPIO27 is the LCD backlight on this board.

### LVGL / display library

Use the BSP's own `bsp_display_start()` + `esp_lvgl_port`. Do NOT pull `rzeldent/esp32-smartdisplay` or other Arduino-Core display libraries — those don't support the P4 RGB panel pipeline. The LCD framebuffer must live in PSRAM (panel uses the LCD_CAM peripheral with PSRAM DMA on the P4).

### External temperature sensor

AM2301 (DHT22-compatible). Read every 30 s, broadcast to web clients as `outdoor_temp`. Currently not wired in `app_main` (see migration status note above).

---

## Display Implementation (`main/p4display.cpp`)

### Layout (800×480 landscape)

Three horizontal bands:
- **Top bar** (height 55 px): Truma logo, outdoor temp, settings button, WiFi + LIN status dots.
- **Content area** (height 387 px): split vertically at x=370.
  - **Left column** (370 px wide): HEATING section (210 px tall) above FAN section (177 px tall).
  - **Right column** (429 px wide): HOT WATER section (200 px tall) above SOLAR/BATTERY section (187 px tall).
- **Status bar** (height 38 px): logo, SSID + IP, status message.

The colour palette is defined as `C_BG / C_PANEL / C_BORDER / C_ACCENT / …` constants at the top of `p4display.cpp`.

### Public API

```cpp
void p4DisplayInit();                       // call once before any LVGL user
void p4DisplayUpdate(const P4DisplayData&); // refreshes the whole UI; locks LVGL internally
void p4DisplaySetStatus(const char* msg);   // status-bar message
bool lvglLock(uint32_t timeout_ms = portMAX_DELAY);
void lvglUnlock();
```

`P4DisplayData` carries temperatures (use `< -100.0f` as "no data" sentinel), heating/fan/boiler/energy state, WiFi/LIN flags, SSID/IP strings, and embedded `P4SolarData` + `P4BattData` structs.

### Behaviour rules (carried over from the C5 implementation, will be re-implemented for P4)

- Toggling heat OFF forces fan to "off".
- Activating boiler forces fan to "off" (avoids unintended fan starts when the boiler cycles).
- Boiler buttons: off / 40 °C / 60 °C / 60 °C ⚡boost map to `TBoilerSetting`.
- Energy dropdown: Gas / Gas+Elec 850W / Gas+Elec 1700W / Elec 850W / Elec 1700W → `TEnergySelection` index 0–4.

### NVS namespaces

| Namespace | Keys | Content |
|-----------|------|---------|
| `wifi` | `ssid`, `pass` | WiFi credentials |
| `mqtt` | `host`, `port`, `user`, `pass` | MQTT broker config |
| `touchcal` | `data` | Touch calibration matrix (GT911 is factory-calibrated; this key may go unused on P4) |
| `display` | `timeout_idx`, `lang` | Screen timeout option index, language (0=ES, 1=EN) |
| `solar` | `addr`, `key` | Victron BLE MAC + encryption key |
| `batt` | `addr` | Ultimatron BLE MAC |

### Settings screens

Reachable from the ⚙ button in the top bar (will be ported from the old `wifisetup.cpp` flow):
- **WiFi config** — blocking scan + connect; saves to NVS and reboots.
- **MQTT config** — blocking host/port/user/pass; saves to NVS and reboots.
- **Solar / battery** — Victron MAC + encryption key + Ultimatron MAC. Works without BLE compiled in (allows pre-provisioning).
- **Display** — timeout selector (30 s / 1 min / 3 min / never).
- **Language** — Spanish / English; applied immediately.

Settings screens that need to block use the navigation-request pattern: an LVGL callback sets a flag (`s_navRequest`); `loop()` reads it outside the LVGL mutex, takes the lock, runs the blocking screen, then reboots if credentials changed.

---

## Related skills

- **`.claude/skills/pio-idf-p4/SKILL.md`** — build system, IDF 6.0 pitfalls, ESP32-P4 memory layout, ModemManager, corrupted build dir.
- **`.claude/skills/lvgl-fonts/SKILL.md`** — Tiny TTF font loading, FA6 icon subset, adding glyphs, `gen_icon_font.py`, EEZ Studio integration.
- **`.claude/skills/truma-protocol/SKILL.md`** — full Truma LIN frame reference (master/slave frames, byte layouts).
- **`.claude/skills/victronble/SKILL.md`** — Victron Instant Readout BLE protocol.
- **`.claude/skills/ultimatronble/SKILL.md`** — Ultimatron BMS GATT protocol.
- **`.claude/skills/ui-interfaces/SKILL.md`** — coordination between LCD touch UI and the WebSocket web UI (single source of truth in `settings.cpp`).
