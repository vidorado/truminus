# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Language policy

**All project code, comments, identifiers, commit messages, log strings, documentation files (READMEs, SKILL.md, design notes) and PR descriptions MUST be in English.** Conversation with the user can be in Spanish, but anything that lands in the repository is English-only. UI-facing strings remain bilingual through `i18n.cpp` (`TK` enum) — never hardcode Spanish text in source files; add a key and use `t(TK::KEY)`.

## Project Overview

TruMinus is firmware for the **JC4880P443C** board (ESP32-P4) that emulates a Truma CP-Plus D control unit to manage a **Truma Combi D** heater over the LIN bus. Control surfaces: MQTT, WebSocket web UI, serial CLI, and a physical 800×480 LCD with capacitive touch.

Solar charge data (Victron BLE) and battery SOC (Ultimatron BLE) are surfaced both on the LCD and the web UI.

> **Status (2026-05):** the project is mid-migration from the previous ESP32-C5 / NM-CYD-C5 board to the JC4880P443C / ESP32-P4 board. `main/main.cpp` runs the LCD, WiFi (via C6 hosted), BLE supervisor (Victron + Ultimatron), the HTTP/WebSocket server and a minimal LIN scheduler (`main/truma_lin.cpp`, IDF-native, no MQTT). The legacy Arduino-flavoured `settings.cpp` / `trumaframes.cpp` / `waterboost.cpp` / `commandreader.cpp` / `autodiscovery.cpp` are **still dormant** — they use `String`/`AsyncWebServer`/`ArduinoJson`/`mqttClient.publish(...)`, are not in `main/CMakeLists.txt::SRCS`, and will be ported in a later phase to bring MQTT, Home Assistant autodiscovery, water-boost and the serial CLI back online. Treat the codebase as porting-in-progress, not feature-complete.

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

### Web assets are served from LittleFS (8 MB partition)

Files in `data/` are baked into a LittleFS image by `littlefs_create_partition_image()` (`main/CMakeLists.txt`) and flashed to the `littlefs` partition at `0x810000` (`partitions_16MB.csv`).  A `web_assets_prep` `ALL` target in the root `CMakeLists.txt` runs on every build *before* the image is regenerated:

- `scripts/cache_bust.py` — rewrites `?v=<sha1>` querystrings in `data/index.html` (and `url(...)` refs in CSS) so browsers refetch changed assets.
- `scripts/gen_gz.py` — pre-gzips compressible files (`.html/.css/.js/.svg/.json/.txt/.ttf`) as adjacent `<file>.gz`.  Uses `mtime=0` for determinism so `--skip-flashed` (see §"Flash") still detects unchanged images.

`main/webserver.cpp::serveFile()` prefers a sibling `.gz` and adds `Content-Encoding: gzip`.  `data/*.gz` is gitignored.

Updating only the web image (no firmware reflash) is supported: `idf.py littlefs-flash-littlefs`.

The previous `compress_fs.py` flash-embed pipeline (which baked everything into `main/webfiles.h`) is gone; the script file remains in `scripts/` but is unused.

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
- **`trumaframes.cpp/.hpp`** — Legacy Arduino-flavoured LIN protocol layer (dormant; not built). Each readable/writable frame class parses raw bytes and publishes to MQTT/WebSocket via `TMqttPublisherBase`. Kept in the tree as the byte-level reference for the future MQTT/autodiscovery port. See `.claude/skills/truma-protocol/SKILL.md`.
- **`truma_lin.cpp/.hpp`** — Active IDF-native LIN scheduler. Emulates the CP-Plus D control unit on `UART_NUM_1 @ 9600` baud (J5, TX=GPIO26 / RX=GPIO27). Reads setpoints from `p4GetControlState()`, writes 7 setpoint frames + the 0x20 control frame each cycle, alternates two master requests (`0xB8` OnOff, `0xB2` GetErrorInfo) over the 0x3C / 0x3D transport, and reads frames 0x21 (room/water temp) + 0x22 (water heating). Exposes a thread-safe `TrumaLinSnapshot` consumed by the main loop. No MQTT, no autodiscovery, no waterboost — those still belong to the dormant Arduino code path.
- **`lin_driver.cpp/.hpp`** — Low-level half-duplex LIN driver over ESP-IDF `driver/uart.h` (UART_NUM_1 recommended; UART_NUM_0 is the console).
- **`settings.cpp/.hpp`** — Setpoint abstraction. `TBoilerSetting`, `TTempSetting`, `TFanSetting`, `TOnOffSetting`, all derived from `TMqttSetting / TAutoDiscovery`. Single source of truth for values consumed by `main.cpp`'s LIN write loop and broadcast back to MQTT/WS/LCD.
- **`globals.hpp`** — shared `mqttClient`, `ws`, MQTT base topics, Home Assistant autodiscovery identifiers.
- **`autodiscovery.cpp/.hpp`** — Home Assistant MQTT discovery payloads. Enabled via `-DAUTODISCOVERY`.
- **`webserver.cpp/.hpp`** — IDF-native HTTP + WebSocket server on `esp_http_server`. Streams files from `/littlefs/` in 1 KB chunks (with `.gz` content-encoding fallback). WS handler parses JSON `{id,value}` frames via `cJSON` and dispatches to a `WsCommandCb`; outgoing frames go through a FreeRTOS queue (`wsQueue`, 48 × 256 B) drained by `wsQueueDrain()`. Native `httpd_ws_send_frame_async` to all `HTTPD_WS_CLIENT_WEBSOCKET` fds returned by `httpd_get_client_list`. Client count tracked via `ws_post_handshake_cb` + `close_fn` (the URI handler is **not** invoked for the handshake leg — see Gotchas).
- **`waterboost.cpp/.hpp`** — 40-minute high-temperature water boost cycle when boiler mode is "boost".
- **`commandreader.cpp/.hpp`** — Serial CLI line buffer for `main.cpp`.
- **`victronble.cpp/.hpp`** — Victron Solar Charger BLE listener (Instant Readout). Uses NimBLE 2.x; stubbed when `-DENABLE_BLE` is absent. See `.claude/skills/victronble/SKILL.md`.
- **`ultimatronble.cpp/.hpp`** — Ultimatron LiFePO4 BMS BLE listener (GATT). See `.claude/skills/ultimatronble/SKILL.md`.
- **`i18n.cpp/.hpp`** — `TK` enum + `t(TK::KEY)`. Spanish (default) / English; language persisted in NVS.
- **`logs.hpp`** — Logging macros / tag conventions.

### Web Interface (`data/`)

Assets live in `data/` and are served from LittleFS (see §"Web assets are served from LittleFS"). `script.js` communicates with the firmware over a WebSocket at `/ws` using JSON envelopes (`{"command": "...", "id": "...", "value": "..."}`).

**Layout mirrors the LCD** (see `main/p4display.cpp`): an 800-px canvas with two rows whose column widths reproduce the LCD's HEAT/HOT-WATER/empty (301/350/149 px) and FAN/SOLAR/empty (213/304/283 px) splits. Encoded as `grid-template-columns` percentages so the canvas can shrink (`--lcd-w: 640px`) while keeping LCD geometry. Container queries (`container-type: inline-size` + `clamp(…, Xcqi, …)`) scale title font sizes proportionally. The Montserrat TTFs the LCD uses live in `data/` too and are served as `@font-face` for the typographic `truminus` logo.

## Cross-cutting gotchas (write these down once, save the next session)

- **`littlefs_create_partition_image()` rebuilds the bin on every cmake run** — CMake can't watch directory contents, so the helper uses `add_custom_target ... ALL` with no input deps. The bin gets a fresh mtime each time even when `data/` is identical. Mitigated by `--skip-flashed` (esptool MD5-compare; baked into every flash target's `SUB_ARGS` in the root `CMakeLists.txt`). Inputs are deterministic (`littlefs-python` + `mtime=0` in our `gen_gz.py`) so the comparison succeeds.
- **VSCode IDF extension's Flash button bypasses our CMake hooks.** It hardcodes the esptool args in TypeScript (`r.push("write_flash","--flash_mode",mode,…)`), ignores `write_flash_args` in `flasher_args.json`, and offers no setting for extra args. We work around by providing `.vscode/tasks.json` tasks ("TruMinus: Flash"/"Flash + Monitor") that invoke `idf.py flash` (which *does* honour our `--skip-flashed`) and `.vscode/settings.json` `VsCodeTaskButtons.tasks` puts status-bar buttons next to them. Recommend `spencerwmiles.vscode-task-buttons` in `extensions.json`. The tasks must source `$IDF_PATH/export.sh` first because `idf.py` is not on PATH in a fresh shell.
- **NimBLE 2.x `scan->start(duration, false)` is non-blocking** — returns once the GAP procedure is queued. Calling `cb(results, count)` right after gives `count=0` every time. Poll `scan->isScanning()` (with `vTaskDelay`) until the duration window elapses, then report. See `main/victronble.cpp::discovery_scan_task`.
- **`esp_http_server` does not invoke the URI handler for the WS upgrade leg.** The handshake is handled internally before any handler runs (`components/esp_http_server/src/httpd_uri.c:362`: *"If the request is websocket handshake, then do not call the uri->handler"*). `req->method` stays `HTTP_GET` for every subsequent frame, so a `if (req->method == HTTP_GET) handshake()` branch in the handler will eat every frame without reading it. Use `CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y` + `ws_post_handshake_cb` to detect new clients. Use `close_fn` for disconnects.
- **`httpd_get_client_list(handle, &fds, fds_array)` requires `fds_array` ≥ `max_open_sockets`** — otherwise returns `ESP_ERR_INVALID_ARG` silently and the WS broadcast loop sees zero clients. Define a single `MAX_OPEN_SOCKETS` constant and use it for both `cfg.max_open_sockets` and the local `int fds[…]` (see `main/webserver.cpp`).
- **`MBEDTLS_ERR_SSL_ALLOC_FAILED (-0x3B00)` + `assert ... sdio_rx_get_buffer ... sdio_drv.c:953` are the same bug: internal DRAM exhaustion.** When the WSS tunnel reconnects right after `IP_EVENT_STA_GOT_IP`, the TLS handshake tries to allocate large chunks; if mbedtls is pinned to internal DRAM the heap drains, mbedtls returns `-0x3B00`, and a moment later the C6 SDIO RX path can't get a DMA-capable buffer and asserts. The three knobs that prevent it are pinned in `sdkconfig.defaults` with a comment: `CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC=y` (was `INTERNAL_MEM_ALLOC`), `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048` (was the IDF default 16384), `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=4096`. Don't disable the tunnel as a "fix" — that's masking the heap, not solving it. See `.claude/skills/pio-idf-p4/SKILL.md` §"DRAM exhaustion cascade".
- **`/dev/ttyACM0` is the P4's native USB-Serial-JTAG, not UART0.** UART0 is on header pins, not wired on this board. Consequences: (a) `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` must be the **primary** console (pinned in `sdkconfig.defaults`), not just secondary — otherwise log output reaches picocom (via secondary duplication) but stdin doesn't; (b) any `esp_console` REPL must be created with `esp_console_new_repl_usb_serial_jtag()` (see `main/cli.cpp`); the UART variant binds input to a port no one is connected to. Also: USB-Serial-JTAG doesn't answer linenoise's ANSI cursor-position probe, so `linenoiseSetDumbMode(1)` is required — line editing degrades to char-by-char with BACKSPACE + Ctrl+U, no arrows / history. This is a hardware limitation of the JTAG endpoint, not a bug.
- **`p4display.cpp::st` mutations need the LVGL lock.** The remote setters (`p4SetHeating`, `p4SetFanMode`, `p4SetBoilerMode`, `p4SetEnergyIdx`, `p4SetRoomSetpoint`, `p4SetScreenTimeoutIdx`) take `bsp_display_lock(50)`; the on-screen button callbacks already run on the LVGL task with the lock held. The diff-broadcast helper (`broadcastControlChanges` in `main.cpp`) reads `st` via `p4GetControlState` from the `wsPumpTask` *without* the lock — safe because each field is a plain int/bool/float, but don't introduce composite reads.

## Key Design Patterns

- **Conditional compilation:** `WEBSERVER`, `AUTODISCOVERY`, `NO_MQTT`, `JC4880_P4`, `ENABLE_BLE`, `ENABLE_SOLAR_DUMMY`, `ENABLE_BOILER_DUMMY` flags gate whole features. `WEBSERVER`, `NO_MQTT` and `JC4880_P4` are pinned in the root `CMakeLists.txt` via `idf_build_set_property(COMPILE_DEFINITIONS … APPEND)` so they're visible to every component.
- **Settings flow:** external input (MQTT / WebSocket / serial CLI / touch) → `settings.cpp` validates → `main.cpp` loop reads value → writes to the right Truma LIN frame → Truma responds → frame published back to MQTT/WS/LCD.
- **LIN bus task:** the LIN UART task is pinned to **Core 0** so blocking serial reads don't fight with WiFi/MQTT/LVGL on Core 1. ESP32-P4 is dual-core (RV32IMAFC), so the pinning model from the prior C5 port still applies.
- **MQTT publish throttling:** values are published on change or after a 10 s timeout to avoid flooding the broker.
- **LVGL locking:** `lv_timer_handler()` runs on a dedicated FreeRTOS task. Any LVGL access from `app_main` / `loop` / `lin_task` must be wrapped in `lvglLock()` / `lvglUnlock()`. Prefer a short timeout (e.g. 10 ms) over `portMAX_DELAY` to avoid deadlocks if the LVGL task is busy.
- **Splash boot ordering:** `app_main` only inits NVS + netif + `p4DisplayInit()` and then spawns `bootTask` (prio 5, 6 KB) for WiFi/C6-OTA/BLE/web. The splash becomes visible as soon as `bsp_display_start_with_config()` returns, and the 2 s minimum-splash in `p4DisplayUpdate()` acts as a *floor* — long inits don't get an extra 2 s artificial delay on top.
- **LCD ↔ web sync (until `settings.cpp` is ported):** dedicated `wsPumpTask` (prio 3, 100 ms cadence) reads `p4GetControlState()`, diffs against the previous snapshot, queues a `{"command":"setting","id":"/x","value":"…"}` per changed field, and drains `wsQueue`. Browser → firmware uses `onWsCommand` in `main.cpp` to route to the `p4Set*()` setters; `onWsConnected` pushes a full snapshot (heating/fan/boiler/temp/energy_idx + ssid) when the browser sends the `"settings"` text frame.

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

LIN UART pins and the AM2301/DHT22 external temperature sensor pin live in `main/main.cpp`. On the JC4880-P4 board the LIN bus is wired to **connector J5 → TX=GPIO26 / RX=GPIO27 on UART_NUM_1 @ 9600 baud** (see `LIN_TX_PIN`/`LIN_RX_PIN` in `main/main.cpp` and `trumaLinStart()` in `main/truma_lin.cpp`). The LCD backlight is on GPIO23 (`CONFIG_BSP_JC4880P443C_LCD_BL_GPIO=23`), not GPIO27 as a previous draft of this document claimed. The AM2301/DHT pin is not yet assigned on the P4 board; the sensor task is dormant.

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

## WSS reverse tunnel — Plesk bridge

`main/wstunnel.cpp/.hpp` dials out to a Plesk-hosted Node.js bridge
(`server/app.js`) over WSS and multiplexes browser ↔ ESP TCP streams as
binary WS frames so the local `esp_http_server` is reachable through
CGNAT.  Settings screen ("Túnel" in the menu) takes only a domain and a
shared token; the firmware composes `wss://<domain>/tunnel?token=<token>`
itself.  A cloud icon in the topbar reflects state: blinking blue while
connecting (500 ms cadence via an LVGL timer), solid `rgb(70,131,210)`
when up, red after 3 consecutive disconnects without a successful
connect.

Wire protocol — control frames are text JSON, bulk data is binary:

```
ESP   → srv  {"type":"hello","node":"truminus","token":"<shared>"}   text
srv   → ESP  {"type":"open", "id":<n>}                                text
both         <4-byte BE id><payload bytes…>                          binary
both         {"type":"close","id":<n>}                                text
```

Binary data frames save ~33 % bandwidth and a parse/format roundtrip
vs. the JSON+base64 phase-1 protocol; both sides must match.

### Local httpd is split into two instances
`pick_local_port()` in `wstunnel.cpp` sniffs the first request bytes and
routes `GET /ws` to the WS-dedicated httpd at `127.0.0.1:81`,
everything else to the asset httpd at `127.0.0.1:80`.  The split exists
to isolate WebSocket sessions from `lru_purge_enable` eviction during
asset stampedes — see `.claude/skills/wss-tunnel/SKILL.md` for the full
rationale.

### Gating, retries, pending queue
- Tunnel only starts on `IP_EVENT_STA_GOT_IP`; `WIFI_EVENT_STA_DISCONNECTED`
  tears the WS down.  Icon stays grey until WiFi is up.
- `esp_websocket_client` auto-reconnects (5 s).  We count consecutive
  `WEBSOCKET_EVENT_DISCONNECTED` and flip the icon red after 3.
- Each tunneled stream consumes 1 local LWIP fd (our client to
  `127.0.0.1:80` or `:81`) + 1 httpd-accepted fd, and 2 TCP PCBs.  When
  all 12 stream slots are busy, new `open` frames go to a pending queue
  with per-id buffer for any `data` bytes that arrive before the open
  materialises (the server pushes the GET line right after the open, so
  we must not drop those bytes).  5 s timeout per pending entry.
- `evict_idle_locked()` reclaims the least-recently-active non-WS stream
  when the table is full, so browser keep-alive zombies cannot starve
  new opens.  WS streams are exempt (`Stream::is_ws`).
- `send_text` / `send_bin` wait up to 10 s on the ws-client internal
  lock — long enough that TLS write backpressure flows back into the
  pump task rather than dropping bytes.  On real failure (peer gone) the
  stream is closed cleanly and the server synthesises a 502 to the
  browser.
- Loopback sockets carry `TCP_NODELAY=1` — Nagle adds multi-second
  latency for the small WS frames that flow ESP→browser through the
  tunnel.  See the skill for the failure mode.
- The wsReaper task in `webserver.cpp` sends a real WS PING (opcode
  0x9) every 20 s to every WS fd across both httpd instances, so
  Plesk's 60 s `proxy_read_timeout` never kills an idle WS.

### Capacity dials
- `MAX_STREAMS=12`, `PENDING_QUEUE_SIZE=16`, `IO_CHUNK=4096`
  (`main/wstunnel.cpp`).
- `httpServer:80 max_open_sockets=12`, `wsServer:81 max_open_sockets=4`,
  `WS_MAX_CLIENTS=8`, `wsCfg.ctrl_port` must differ from `cfg.ctrl_port`
  (`main/webserver.cpp`).
- `esp_websocket_client cfg.buffer_size=8192` — ≥ IO_CHUNK + 4.
- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048` so mbedtls / PSA contexts
  land in PSRAM at boot, leaving internal DRAM for AES-GCM DMA buffers.
- `CONFIG_LWIP_MAX_SOCKETS=40`, `CONFIG_LWIP_MAX_ACTIVE_TCP=64` —
  loopback streams + their 60 s TIME_WAIT lingering blow through the
  default 16 PCBs immediately.
- `CONFIG_HTTPD_MAX_REQ_HDR_LEN=4096`, `CONFIG_HTTPD_MAX_URI_LEN=1024` —
  modern browsers carry ~2 KB of headers; the default 512 / our previous
  1024 returned HTTP 431 "Header fields are too long" through the tunnel.

### NVS namespace `tunnel`
- `enabled` (u8) — 0/1
- `server` (str) — bare domain, e.g. `truminus.victordorado.es`.  The
  firmware strips any `wss://…/path?…` the user might paste, then
  rebuilds the URL with the canonical `/tunnel` path and `?token=…`.
- `token` (str) — shared secret matching `TUNNEL_TOKEN` env on the
  server.

### Server side (`server/app.js`)
Single Node.js app on Plesk → Node.js.  Uses `http.createServer()` (so
Phusion Passenger's `listen()` hook fires and the spawn doesn't time
out), then replaces the default `connection` listener with a sniffer:
`GET /tunnel … Upgrade: websocket` is routed back through Node's HTTP
parser so the `WebSocketServer` can complete the handshake;
everything else is muxed as opaque bytes through the device control
channel.  When the device disconnects, in-flight tunneled sockets are
closed with a synthetic 502 instead of `socket.destroy()` so Passenger
doesn't surface "Incomplete response received from application".

## Related skills

- **`.claude/skills/pio-idf-p4/SKILL.md`** — build system, IDF 6.0 pitfalls, ESP32-P4 memory layout, ModemManager, corrupted build dir.
- **`.claude/skills/lvgl-fonts/SKILL.md`** — Tiny TTF font loading, FA6 icon subset, adding glyphs, `gen_icon_font.py`, EEZ Studio integration.
- **`.claude/skills/truma-protocol/SKILL.md`** — full Truma LIN frame reference (master/slave frames, byte layouts).
- **`.claude/skills/victronble/SKILL.md`** — Victron Instant Readout BLE protocol.
- **`.claude/skills/ultimatronble/SKILL.md`** — Ultimatron BMS GATT protocol.
- **`.claude/skills/ui-interfaces/SKILL.md`** — coordination between LCD touch UI and the WebSocket web UI (single source of truth in `settings.cpp`).
- **`.claude/skills/wss-tunnel/SKILL.md`** — WSS reverse-tunnel operational design, sizing dials, and a troubleshooting quick-reference for tunnel-only failures (Nagle on loopback, LRU eviction of WS, mbedtls/PSA heap, …).
