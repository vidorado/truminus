# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Language policy

**All project code, comments, identifiers, commit messages, log strings, documentation files (READMEs, SKILL.md, design notes) and PR descriptions MUST be in English.** Conversation with the user can be in Spanish, but anything that lands in the repository is English-only. UI-facing strings remain bilingual through `i18n.cpp` (`TK` enum) — never hardcode Spanish text in source files; add a key and use `t(TK::KEY)`.

## Project Overview

TruMinus is firmware for the **JC4880P443C** board (ESP32-P4) that emulates a Truma CP-Plus D control unit to manage a **Truma Combi D** heater over the LIN bus. Control surfaces: MQTT, WebSocket web UI, serial CLI, and a physical 800×480 LCD with capacitive touch. Solar charge data (Victron BLE) and battery SOC (Ultimatron BLE) are surfaced on both the LCD and the web UI.

> **Status (2026-05):** mid-migration from the previous ESP32-C5 / NM-CYD-C5 board to the JC4880P443C / ESP32-P4 board. `main/main.cpp` runs the LCD, WiFi (via C6 hosted), BLE supervisor, the HTTP/WebSocket server and a minimal IDF-native LIN scheduler (`main/truma_lin.cpp`, no MQTT). The legacy Arduino-flavoured `settings.cpp` / `trumaframes.cpp` / `waterboost.cpp` / `commandreader.cpp` / `autodiscovery.cpp` are **still dormant** (use `String`/`AsyncWebServer`/`ArduinoJson`/`mqttClient`, not in `main/CMakeLists.txt::SRCS`) and will be ported later to bring MQTT, Home Assistant autodiscovery, water-boost and the serial CLI back online. Treat the codebase as porting-in-progress, not feature-complete.

## Build System

Driven by **`idf.py`** (ESP-IDF 6.0.1) via a thin `Makefile`. PlatformIO is not used.

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

VSCode: install `espressif.esp-idf-extension`, copy `.vscode/settings.json.template` → `.vscode/settings.json` and fill in your IDF path. IntelliSense reads `build/compile_commands.json`.

**Before touching the build** (sdkconfig, components, link errors, IRAM overflow, corrupted `build/`, ModemManager, CI caching, the VSCode Flash button), **read `.claude/skills/pio-idf-p4/SKILL.md`** — all the IDF 6.0 + ESP32-P4 pitfalls live there.

### Web assets are served from LittleFS (8 MB partition)

Files in `data/` are baked into a LittleFS image by `littlefs_create_partition_image()` (`main/CMakeLists.txt`) and flashed to the `littlefs` partition at `0x810000`. A `web_assets_prep` `ALL` target runs on every build *before* the image is regenerated: `scripts/cache_bust.py` rewrites `?v=<sha1>` querystrings so browsers refetch changed assets, and `scripts/gen_gz.py` pre-gzips compressible files as adjacent `<file>.gz` (`mtime=0` for determinism). `serveFile()` prefers a sibling `.gz` with `Content-Encoding: gzip`; `data/*.gz` is gitignored. Web-only reflash: `idf.py littlefs-flash-littlefs`. (The old `compress_fs.py` → `main/webfiles.h` embed pipeline is gone; the script remains but is unused.)

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

ESP-IDF native convention: no `src/`; all firmware sources live in `main/`.

- **`main.cpp`** — `app_main` entry point: inits NVS + netif + display, spawns `bootTask` for WiFi/C6-OTA/BLE/web. Hosts `wsPumpTask`, the WS command router (`onWsCommand`) and the LCD↔web diff broadcaster.
- **`p4display.cpp/.hpp`** — LVGL UI for the 800×480 LCD. See §"Display Implementation".
- **`truma_lin.cpp/.hpp`** — Active IDF-native LIN scheduler. Emulates the CP-Plus D on `UART_NUM_1 @ 9600`, writes 7 setpoint frames + the 0x20 control frame each cycle, alternates two master requests (`0xB8` OnOff, `0xB2` GetErrorInfo) over 0x3C/0x3D, reads frames 0x21/0x22. Thread-safe `TrumaLinSnapshot`. No MQTT/autodiscovery/waterboost. See `.claude/skills/truma-protocol/SKILL.md`.
- **`lin_driver.cpp/.hpp`** — Low-level half-duplex LIN driver over `driver/uart.h`.
- **`trumaframes.cpp/.hpp`** — Legacy Arduino LIN protocol layer (dormant; not built). Byte-level reference for the future MQTT/autodiscovery port. See the truma-protocol skill.
- **`settings.cpp/.hpp`** — Setpoint abstraction (`TBoilerSetting`/`TTempSetting`/`TFanSetting`/`TOnOffSetting`), dormant until ported.
- **`globals.hpp`** — shared `mqttClient`, `ws`, MQTT base topics, HA autodiscovery identifiers.
- **`autodiscovery.cpp/.hpp`** — Home Assistant MQTT discovery payloads (`-DAUTODISCOVERY`).
- **`webserver.cpp/.hpp`** — IDF-native HTTP + WebSocket server on `esp_http_server`. Streams files from `/littlefs/` in 16 KB PSRAM chunks with `.gz` fallback; WS JSON `{id,value}` frames via `cJSON`; outgoing frames via the `wsQueue` FreeRTOS queue. Dead-fd reaping and httpd gotchas: see `.claude/skills/wss-tunnel/SKILL.md`.
- **`victronble.cpp/.hpp`** — Victron Solar Charger BLE (Instant Readout, NimBLE 2.x). See `.claude/skills/victronble/SKILL.md`.
- **`ultimatronble.cpp/.hpp`** — Ultimatron LiFePO4 BMS BLE (GATT). See `.claude/skills/ultimatronble/SKILL.md`.
- **`tankble.cpp/.hpp`** — Fresh-water tank level via BTHome v2 service-data (UUID `0xFCD2`, moisture tag `0x2F`), gated by NVS `tank/addr`. Piggybacks on `VictronScanCb::onResult`. Exposes `TankData`, WS `{"command":"tank",…}`.
- **`multiplusble.cpp/.hpp`** — Victron VE.Bus / Multiplus Instant Readout (record `0x0C`). Read-only (no documented VE.Bus GATT). Exposes `MultiplusData`, WS `{"command":"multi",…}`. See `.claude/skills/multiplusble/SKILL.md`.
- **`p4_ota.cpp/.hpp`** — Self-OTA for the P4 application image (distinct from `c6_ota.cpp`). See `.claude/skills/firmware-ota/SKILL.md`.
- **`wstunnel.cpp/.hpp`** — WSS reverse tunnel (CGNAT traversal). See `.claude/skills/wss-tunnel/SKILL.md`.
- **`am2301.cpp`** — AM2301/DHT22 outdoor temperature on GPIO52 via RMT. See pio-idf-p4 SKILL §14.
- **`i18n.cpp/.hpp`** — `TK` enum + `t(TK::KEY)`. Spanish (default) / English, persisted in NVS.
- **`waterboost.cpp/.hpp`**, **`commandreader.cpp/.hpp`** — dormant (40-min boost cycle; serial CLI line buffer).
- **`logs.hpp`** — logging macros / tag conventions.

### Web Interface (`data/`)

Assets in `data/` served from LittleFS; `script.js` talks to the firmware over a WebSocket at `/ws` using JSON envelopes (`{"command","id","value"}`). PWA support via `data/icons/site.webmanifest` (`display: standalone`). Topbar status dots: cloud (tunnel), Bluetooth (BLE), WiFi, LIN — driven by `{"command":"icon","id":"ble|tunnel","state":N}`.

Layout, responsive breakpoints, panel semantics (SOLAR/BATERÍA/INVERSOR), CSS gotchas and the LCD↔web protocol all live in `.claude/skills/ui-interfaces/SKILL.md`.

## Key Design Patterns

- **Conditional compilation:** `WEBSERVER`, `AUTODISCOVERY`, `NO_MQTT`, `JC4880_P4`, `ENABLE_BLE`, `ENABLE_SOLAR_DUMMY`, `ENABLE_BOILER_DUMMY` gate whole features. `WEBSERVER`, `NO_MQTT`, `JC4880_P4` are pinned in the root `CMakeLists.txt` (visible to every component).
- **Settings flow:** external input (MQTT / WS / serial / touch) → `settings.cpp` validates → `main.cpp` loop writes the right LIN frame → Truma responds → frame published back to MQTT/WS/LCD.
- **Task pinning:** the LIN UART task is pinned to **Core 0** so blocking serial reads don't fight WiFi/MQTT/LVGL on Core 1 (P4 is dual-core RV32IMAFC).
- **LVGL locking:** `lv_timer_handler()` runs on a dedicated task. Any LVGL access from `app_main` / `loop` / `lin_task` must be wrapped in `lvglLock()` / `lvglUnlock()` — prefer a short timeout (e.g. 10 ms) over `portMAX_DELAY`. The `st`-mutation lock contract is in the ui-interfaces skill.
- **Splash boot ordering:** `app_main` inits NVS + netif + `p4DisplayInit()` then spawns `bootTask` (prio 5); the 2 s minimum-splash acts as a floor, not an added delay.
- **MQTT publish throttling:** values published on change or after a 10 s timeout.

## MQTT Topics

Base topics in `globals.hpp` — status `truma/status/<field>`, setpoints `truma/set/<field>`. Writable setpoints: `temp`, `heating`, `boiler` (off/eco/high/boost), `fan` (off/eco/high/1–10), `energy_idx` (0–4), `simultemp`, `error_reset`, `refresh`, `ping`.

---

## Target Hardware — JC4880P443C (ESP32-P4)

- **MCU:** ESP32-P4-WROOM (RISC-V dual-core RV32IMAFC, 400 MHz). The EVB module here is rev < v3 (`chip_variant: "esp32p4_es"`), which enables `--enable-non-contiguous-regions` on the linker — see pio-idf-p4 SKILL §4.
- **Memory:** 16 MB Flash (QIO @ 80 MHz) / 32 MB PSRAM (HEX @ 200 MHz).
- **Display:** 4.3" **ST7701** RGB panel, 800×480 landscape (`esp_lcd_st7701`). Framebuffer must live in PSRAM. Use the BSP's `bsp_display_start()` + `esp_lvgl_port`; do **not** pull Arduino-Core display libraries.
- **Touch:** **GT911** on the shared I2C bus (`BSP_I2C_SCL=GPIO8`, `BSP_I2C_SDA=GPIO7`); RST/INT not wired (`BSP_LCD_TOUCH_RST = GPIO_NUM_NC`).
- **Connectivity:** WiFi + BLE 5 via a separate ESP32-C6 co-processor over SDIO/SPI (see `components/jc4880_bsp/WIFI_ARCHITECTURE.md`).
- **BSP:** vendored at `components/jc4880_bsp/` (forked from `csvke/esp32_p4_jc4880p433c_bsp`); pulls `esp_lcd_st7701`, `esp_lcd_touch_gt911`, `esp_lvgl_port`, `lvgl` from the registry on first build.
- **Upload:** USB-CDC on `/dev/ttyACM0` (P4 exposes USB natively; the JTAG console gotcha is in pio-idf-p4 SKILL §7). Speed 460800.

### Pin assignments

LIN bus on **connector J5 → TX=GPIO27 / RX=GPIO26, UART_NUM_1 @ 9600** (`LIN_TX_PIN`/`LIN_RX_PIN` in `main/main.cpp`). LCD backlight on GPIO23 (`CONFIG_BSP_JC4880P443C_LCD_BL_GPIO=23`). AM2301/DHT22 outdoor sensor DATA on **GPIO52** (`AM2301_DATA_PIN`), read via RMT. Audio I2S codec wired (SCLK=12/MCLK=13/LCLK=10/DOUT=9/DSIN=48/PA=11) but unused.

---

## Display Implementation (`main/p4display.cpp`)

Three horizontal bands at 800×480: top bar (55 px: logo, outdoor temp, ⚙, WiFi+LIN dots), content area (387 px, split at x=370), status bar (38 px: logo, SSID+IP, status message). Colour palette = `C_BG / C_PANEL / C_BORDER / C_ACCENT / …` constants at the top of the file.

```cpp
void p4DisplayInit();                       // call once before any LVGL user
void p4DisplayUpdate(const P4DisplayData&); // refreshes whole UI; locks LVGL internally
void p4DisplaySetStatus(const char* msg);   // status-bar message
bool lvglLock(uint32_t timeout_ms = portMAX_DELAY);
void lvglUnlock();
```

`P4DisplayData` carries temperatures (`< -100.0f` = "no data" sentinel), heating/fan/boiler/energy state, WiFi/LIN flags, SSID/IP, and embedded `P4SolarData` + `P4BattData`. Layout details, panel semantics and behaviour rules (heat-off forces fan off, boiler activation forces fan off, boiler/energy mappings) are in `.claude/skills/ui-interfaces/SKILL.md`.

### NVS namespaces

| Namespace | Keys | Content |
|-----------|------|---------|
| `wifi` | `ssid`, `pass` | WiFi credentials |
| `mqtt` | `host`, `port`, `user`, `pass` | MQTT broker config |
| `display` | `timeout_idx`, `lang` | Screen timeout index, language (0=ES, 1=EN) |
| `solar` | `addr`, `key` | Victron BLE MAC + encryption key |
| `batt` | `addr` | Ultimatron BLE MAC |
| `tank` | `addr` | Tank BTHome sensor MAC (moisture tag 0x2F). Empty = disabled. |
| `multiplus` | `addr`, `key` | VE.Bus dongle MAC + per-device AES-128 bind key. Empty = panel hidden. |
| `tunnel` | `enabled`, `server`, `token`, `pass` | WSS tunnel config + BasicAuth password for tunneled web access (see wss-tunnel skill) |
| `ota` | `rb_why`, `rb_heap` | Last rollback reason + heap (see firmware-ota skill) |

### Settings screens (⚙ button)

WiFi config · MQTT config · Monitorización (Victron/Ultimatron/tank/Multiplus MACs+keys, each with a 🔍 BLE-discovery button) · Display (timeout) · Language · Túnel · Actualizaciones. Blocking screens use the navigation-request pattern: an LVGL callback sets `s_navRequest`; `loop()` reads it outside the LVGL mutex, takes the lock, runs the screen, reboots if credentials changed.

---

## Related skills

- **`pio-idf-p4`** — build system, IDF 6.0 pitfalls, P4 memory layout, ModemManager, corrupted build dir, USB-Serial-JTAG console, CI caching, `PROJECT_VER`, WiFi power save, LittleFS/Flash-button, RMT open-drain, DRAM exhaustion cascade.
- **`firmware-ota`** — P4 self-OTA: GitHub-Releases-direct discovery, versioning, auto-prompt policy, transfer tuning, PENDING_VERIFY self-test/rollback net.
- **`wss-tunnel`** — WSS reverse tunnel (firmware side): two-httpd split, Nagle on loopback, LRU eviction, mbedtls/PSA heap, plus the local httpd/WS server gotchas. Bridge side: companion repo [`vidorado/truminus-cloud-server`](https://github.com/vidorado/truminus-cloud-server) → `tunnel-bridge` skill.
- **`ui-interfaces`** — LCD touch UI ↔ WebSocket web UI coordination, layout, responsive CSS gotchas, panel semantics.
- **`lvgl-fonts`** — Tiny TTF font loading, FA6 icon subset, `gen_icon_font.py`, EEZ Studio.
- **`truma-protocol`** — full Truma LIN frame reference (master/slave frames, byte layouts).
- **`victronble`** — Victron Instant Readout BLE (Solar / SmartShunt / BMV).
- **`multiplusble`** — Victron VE.Bus / Multiplus Instant Readout (record 0x0C).
- **`ultimatronble`** — Ultimatron BMS GATT protocol.
