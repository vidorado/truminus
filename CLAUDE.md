# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Language policy

**All project code, comments, identifiers, commit messages, log strings, documentation files (READMEs, SKILL.md, design notes) and PR descriptions MUST be in English.** Conversation with the user can be in Spanish, but anything that lands in the repository is English-only. UI-facing strings remain bilingual through `i18n.cpp` (`TK` enum) — never hardcode Spanish text in source files; add a key and use `t(TK::KEY)`.

## Comment policy

**Comments describe the current state of the code, not its history.** Once a bug is fixed, the comment explains how the code works now — never "this used to do X", "previously failed because…", or "changed from Y". The one exception is a **gotcha that can recur**: a non-obvious constraint a future change could re-break (e.g. "the snapshot must list every field the change-broadcaster covers"). Phrase those as a forward-looking invariant, not a war story. The same applies to commit messages: describe the change, don't narrate the debugging journey.

## Research discipline — don't satisfice

**When the result isn't good, dig into the docs instead of settling.** For *any*
topic touching ESP-IDF or the ESP32-P4 — not just memory/RAM — if a fix didn't
work, a symptom persists after a change, a test still fails, a second attempt
missed, or you're about to answer from memory or a distilled skill without a
cited source, **stop and search the `esp32p4-docs` RAG** (local ESP-IDF docs +
every `CONFIG_*` option; `search.sh <term>` / `config_index.txt`) before you
conclude, retry blindly, or give up. A skill captures what we already learned and
is a *starting point*, not the last word — the docs and Kconfig it was distilled
from are the source of truth and routinely hold a knob, default or
silicon-revision caveat the skill omits (e.g. the ES-silicon fixed brownout
threshold). Prefer citing a doc/Kconfig line over asserting from memory.

## Project Overview

TruMinus is firmware for the **JC4880P443C** board (ESP32-P4) that emulates a Truma CP-Plus D control unit to manage a **Truma Combi D** heater over the LIN bus. Control surfaces: WebSocket web UI, serial CLI, and a physical 800×480 LCD with capacitive touch. Solar charge (Victron BLE), battery SOC (Ultimatron BLE), fresh-water tank level (BTHome), inverter state (Victron VE.Bus) and an OpenAir PLUS A/C are surfaced on both the LCD and the web UI.

> **Status:** the migration off the old ESP32-C5 / NM-CYD-C5 board is complete and the legacy Arduino-flavoured layer is **gone** — `settings.cpp`, `trumaframes.cpp`, `waterboost.cpp`, `commandreader.cpp` and `autodiscovery.cpp` were deleted, not merely dormant. **MQTT and Home Assistant autodiscovery are not implemented** (the build pins `NO_MQTT`); the web UI, LCD and serial CLI are the live control surfaces. Water-boost has no replacement yet. Byte-level LIN protocol reference lives in the truma-protocol skill, not in a dormant source file.

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

### Host tests (`test/host/`)

The IDF-free modules under `main/` — every `*_codec.cpp`, plus `lin_frames`, `lin_protocol`, `mode_controller`, `ws_command`, `ws_diff`, `version_compare` — are compiled natively and run under Catch2. **No ESP-IDF, no hardware, seconds to run**; use them as the fast feedback loop for any logic change.

```bash
cd test/host && ./build.sh && ./build/tests
```

`test/host/CMakeLists.txt` lists those sources **by path**, so moving a module between `main/` subdirectories breaks the suite — update the `add_library` list (and `INCLUDE_DIRS`, which mirrors `main/CMakeLists.txt`) in the same commit. `.github/workflows/tests.yml` runs this on every push so the breakage surfaces immediately; `ci.yml` (the full IDF firmware build) stays `workflow_dispatch`-only.

VSCode: install `espressif.esp-idf-extension`, copy `.vscode/settings.json.template` → `.vscode/settings.json` and fill in your IDF path. IntelliSense reads `build/compile_commands.json`.

**Before touching the build** (sdkconfig, components, link errors, IRAM overflow, corrupted `build/`, ModemManager, CI caching, the VSCode Flash button), **read `.claude/skills/pio-idf-p4/SKILL.md`** — all the IDF 6.0 + ESP32-P4 pitfalls live there.

### Patching managed_components (durable)

`managed_components/` is gitignored and re-downloaded by the IDF component
manager, so editing those files directly does NOT persist. The durable way to
patch a registry component is **`scripts/apply_patches.py`**,
run automatically at cmake-configure time (wired in `CMakeLists.txt`). Each
entry is an idempotent string find/replace (no-op once applied), so it
re-applies after any component re-download. To add one: append an entry to the
`PATCHES` list (target path + old/new strings + desc) and, by convention, drop
a documentation diff in `patches/`. Existing patches cover `esp_lcd_st7701`,
`jc4880_bsp`, `esp_lvgl_port` (GT911 graceful I2C) and `esp-nimble-cpp`
(`findAdvField` OOB-read guard).

### Web assets are served from LittleFS (8 MB partition)

Files in `data/` are baked into a LittleFS image by `littlefs_create_partition_image()` (`main/CMakeLists.txt`) and flashed to the `littlefs` partition at `0x810000`. A `web_assets_prep` `ALL` target runs on every build *before* the image is regenerated: `scripts/cache_bust.py` rewrites `?v=<sha1>` querystrings so browsers refetch changed assets, and `scripts/gen_gz.py` pre-gzips compressible files as adjacent `<file>.gz` (`mtime=0` for determinism). `serveFile()` prefers a sibling `.gz` with `Content-Encoding: gzip`; `data/*.gz` is gitignored. Web-only reflash: `idf.py littlefs-flash-littlefs`. (The old `compress_fs.py` → `main/webfiles.h` embed pipeline is gone, script included.)

## Architecture

### Communication Flow

```
Truma Combi D ←→ LIN transceiver ←→ ESP32-P4 UART
                                       ↕
                              Web clients / Serial CLI / Touch UI
                                       ↕
                              BLE: Victron solar · Ultimatron BMS · tank
                                   Victron VE.Bus · OpenAir PLUS A/C
```

### Source layout (`main/`)

ESP-IDF native convention: no top-level `src/`; all firmware sources live under `main/`, grouped into subdirectories. Every subdirectory is on the include path (`main/CMakeLists.txt::INCLUDE_DIRS`), so headers are included by bare name (`#include "truma_lin.hpp"`), not by relative path.

**A `*_codec.cpp` module is deliberately free of every ESP-IDF dependency** so it can be compiled natively and unit-tested — see §"Host tests". Keep decoding/encoding/derivation logic in those modules and I/O in their non-codec siblings; that split is what makes the logic testable.

**`app/` — composition root**
- **`main.cpp`** (in `main/`) — `app_main`: NVS → faultlog/crashcatch → event loop + netif → log levels → language → display init → `bootStart()`, then a 1 Hz loop calling `displaySyncTick()`.
- **`boot_sequence.cpp/.hpp`** — the background `bootTask`: WiFi/ESP-Hosted, C6 OTA, BT controller, BLE inits, LittleFS, web server, tunnel, `wsPumpTask`, CLI, AM2301, LIN, self-OTA, then NimBLE last. Ordering is deliberate — it sequences heap peaks instead of stacking them.
- **`display_sync.cpp/.hpp`** — per-tick aggregation of every subsystem snapshot into one `P4DisplayData`, plus the status-line alert rotation and the Truma/A-C error modal policy.

**`ui/`**
- **`p4display.cpp/.hpp`** — LVGL UI for the 800×480 LCD. See §"Display Implementation".
- **`p4settings.cpp/.hpp`** — the ⚙ settings screens (WiFi, monitoring MACs, display, language, tunnel, updates) plus the on-screen keyboard.

**`lin/`**
- **`truma_lin.cpp/.hpp`** — Active IDF-native LIN scheduler. Emulates the CP-Plus D on `UART_NUM_1 @ 9600`, writes 7 setpoint frames + the 0x20 control frame each cycle, alternates two master requests (`0xB8` OnOff, `0xB2` GetErrorInfo) over 0x3C/0x3D, reads frames 0x21/0x22. Thread-safe `TrumaLinSnapshot`. See `.claude/skills/truma-protocol/SKILL.md`.
- **`lin_driver.cpp/.hpp`** — Low-level half-duplex LIN driver over `driver/uart.h`.
- **`lin_protocol.cpp`**, **`lin_frames.cpp`**, **`lin_codec.cpp`** — IDF-free PID/checksum, frame bit-packing and value codecs (host-tested).
- **`mode_controller.cpp/.hpp`** — `derive_mode()`: control state → LIN setpoints, plus the fan/boiler string↔int conversions (host-tested).

**`web/`**
- **`webserver.cpp/.hpp`** — IDF-native HTTP + WebSocket server on `esp_http_server`. Streams files from `/littlefs/` in 16 KB PSRAM chunks with `.gz` fallback; WS JSON `{id,value}` frames via `cJSON`; outgoing frames via the `wsQueue` FreeRTOS queue. Dead-fd reaping and httpd gotchas: see `.claude/skills/wss-tunnel/SKILL.md`.
- **`ws_router.cpp`** / **`ws_command.cpp`** — incoming WS frames: routing, and the IDF-free `{id,value}` → action parsing (host-tested).
- **`ws_snapshot.cpp`** — the full-state burst sent to a browser on connect.
- **`ws_broadcaster.cpp`** — `wsPumpTask` (100 ms): change-detected outgoing frames, plus A/C state adoption from unit telemetry.
- **`ws_diff.cpp/.hpp`** — IDF-free "did this value change enough to rebroadcast" predicates (host-tested).
- **`wifi_manager.cpp/.hpp`** — STA bring-up, status, and async AP scan.
- **`wstunnel.cpp/.hpp`** — WSS reverse tunnel (CGNAT traversal). See `.claude/skills/wss-tunnel/SKILL.md`.

> `ws_snapshot.cpp` and `ws_broadcaster.cpp` are two halves of one contract: the broadcaster only emits *on change*, so any state already stable when a client connects reaches it through the snapshot or never. Adding a field means touching both.

**`ble/`** — every driver exposes a mutex-guarded `…GetData()` snapshot and an `…IsConfigured()` NVS gate.
- **`victronble.cpp/.hpp`** — Victron Solar Charger BLE (Instant Readout, NimBLE 2.x); also hosts the shared scan callback and the BLE supervisor task. See `.claude/skills/victronble/SKILL.md`.
- **`ultimatronble.cpp/.hpp`** — Ultimatron LiFePO4 BMS BLE (GATT). See `.claude/skills/ultimatronble/SKILL.md`.
- **`tankble.cpp/.hpp`** — Fresh-water tank level via BTHome v2 service-data (UUID `0xFCD2`, moisture tag `0x2F`), gated by NVS `tank/addr`. Piggybacks on `VictronScanCb::onResult`. Exposes `TankData`, WS `{"command":"tank",…}`.
- **`multiplusble.cpp/.hpp`** — Victron VE.Bus / Multiplus Instant Readout (record `0x0C`). Read-only (no documented VE.Bus GATT). Exposes `MultiplusData`, WS `{"command":"multi",…}`. See `.claude/skills/multiplusble/SKILL.md`.
- **`openairble.cpp/.hpp`**, **`openair_config.cpp/.hpp`** — OpenAir PLUS (Bergstrom/Dirna) A/C over BLE GATT: telemetry, commands, pairing state. `openair_config` caches the NVS "configured" flag that flips the LCD panel between CALEFACCIÓN and CLIMATIZACIÓN. See `.claude/skills/openair-plus/SKILL.md`.
- **`victron_codec.cpp`**, **`ultimatron_codec.cpp`**, **`multiplus_codec.cpp`**, **`bthome_codec.cpp`** — IDF-free advertisement/GATT parsers (host-tested).

**`sensors/`**
- **`am2301.cpp/.hpp`** — AM2301/DHT22 outdoor temperature on GPIO52 via RMT (30 s cadence). See pio-idf-p4 SKILL §14.
- **`am2301_codec.cpp`** — IDF-free pulse-train → temperature/humidity decode (host-tested).

**`ota/`**
- **`p4_ota.cpp/.hpp`** — Self-OTA for the P4 application image, plus the LittleFS web-asset sync and the PENDING_VERIFY self-test/rollback net. See `.claude/skills/firmware-ota/SKILL.md`.
- **`c6_ota.cpp/.hpp`** — reflashes the ESP32-C6 co-processor over SDIO when the embedded slave firmware and the host ESP-Hosted library disagree.
- **`version_compare.cpp/.hpp`** — IDF-free semver comparison (host-tested).

**`core/`**
- **`i18n.cpp/.hpp`** — `TK` enum + `t(TK::KEY)`. Spanish (default) / English, persisted in NVS.
- **`control_state.hpp`** — `P4ControlState`, the single shared control-surface struct.
- **`cli.cpp/.hpp`** — serial REPL on USB-Serial-JTAG (`wifi`, `victron`, `ultimatron`, `tunnel`, `show`, `help`). Live, not dormant.
- **`flags.cpp/.h`** — build-flag definitions + the runtime per-TAG `ESP_LOG` level silencer. Force-included into every `main` TU via `-include`.
- **`faultlog.cpp`** / **`faultlog_codec.cpp`** — persisted reset-reason history (codec host-tested).
- **`crashcatch.cpp/.hpp`** — `--wrap=esp_panic_handler` shim that stashes the crash context (PC/RA/SP/stack) in RTC memory for the next boot to report.
- **`heapdiag.cpp/.hpp`** — `heapDiagMark()` internal-DRAM probes used to track the OTA heap floor.
- **`logs.hpp`** — logging macros / tag conventions.
- **`globals.hpp`** — MQTT base topics + Home Assistant autodiscovery identifiers. **Currently included by nothing**: it is a kept spec reference for the unimplemented MQTT port, not live code. Do not assume anything here is in effect.

### Web Interface (`data/`)

Assets in `data/` served from LittleFS; `script.js` talks to the firmware over a WebSocket at `/ws` using JSON envelopes (`{"command","id","value"}`). PWA support via `data/icons/site.webmanifest` (`display: standalone`). Topbar status dots: cloud (tunnel), Bluetooth (BLE), WiFi, LIN — driven by `{"command":"icon","id":"ble|tunnel","state":N}`.

Layout, responsive breakpoints, panel semantics (SOLAR/BATERÍA/INVERSOR), CSS gotchas and the LCD↔web protocol all live in `.claude/skills/ui-interfaces/SKILL.md`.

## Key Design Patterns

- **Conditional compilation:** `WEBSERVER`, `NO_MQTT`, `JC4880_P4` are pinned in the root `CMakeLists.txt` (visible to every component); `ENABLE_BLE` is set on the `main` component. `ENABLE_BOILER_DUMMY` / `ENABLE_TEMP_DUMMY` inject fake sensor values for bench work. `AUTODISCOVERY` is referenced by no live code.
- **`P4ControlState` is the single source of truth for the control surface.** It lives as `st` inside `p4display.cpp`; LVGL callbacks mutate it directly, and remote setters (`p4SetHeating`, `p4SetRoomSetpoint`, …) take the LVGL lock to mutate it plus refresh the widgets atomically. Everyone else *reads* it via `p4GetControlState()`. Flow: input (WS / serial / touch) → `p4Set*` validates and stores → `lin_task` reads it back through `derive_mode()` and writes the LIN frames → the Truma responds → the snapshot fans back out to LCD and web.
- **Snapshot pattern:** every asynchronous producer (LIN, each BLE driver, AM2301, WiFi, OTA) owns a mutex-guarded struct and exposes a by-value `…GetSnapshot()/…GetData()`. Consumers never reach into producer state.
- **Task pinning:** the LIN UART task is pinned to **Core 0** so blocking serial reads don't fight WiFi/LVGL on Core 1 (P4 is dual-core RV32IMAFC).
- **LVGL locking:** `lv_timer_handler()` runs on a dedicated task. Any LVGL access from `app_main` / `displaySyncTick` / `lin_task` must be wrapped in `lvglLock()` / `lvglUnlock()` — prefer a short timeout (e.g. 10 ms) over `portMAX_DELAY`. The `st`-mutation lock contract is in the ui-interfaces skill.
- **Never call a raw LVGL setter from a periodic refresh path.** `lv_label_set_text()` and every `lv_obj_set_style_*()` invalidate unconditionally — they do *not* short-circuit on an unchanged value — so a 1 Hz whole-UI refresh overflows LVGL's 32-slot dirty-area list and degrades into a full 800×480 repaint every second. `p4display.cpp` provides guarded `set_text` / `set_text_fmt` / `set_bg_color` / `set_text_color` / `set_text_opa` / `set_opa` / `set_height` / `bm_set_ctrl` helpers; use those. (`lv_obj_add_flag`/`add_state`/`lv_bar_set_value` self-guard and are safe raw.)
- **Splash boot ordering:** `app_main` inits NVS + netif + `p4DisplayInit()` then spawns `bootTask` (prio 5); the 2 s minimum-splash acts as a floor, not an added delay.

## MQTT

**Not implemented.** `NO_MQTT` is pinned project-wide and no MQTT client is built or started. The `mqtt` NVS namespace and the settings screen that writes it are leftovers awaiting the port. When it lands, the intended contract (recorded in `core/globals.hpp`) is status on `truma/status/<field>`, setpoints on `truma/set/<field>`, with writable `temp`, `heating`, `boiler` (off/eco/high/boost), `fan` (off/eco/high/1–10), `energy_idx` (0–4), `simultemp`, `error_reset`, `refresh`, `ping`.

---

## Target Hardware — JC4880P443C (ESP32-P4)

- **MCU:** ESP32-P4-WROOM (RISC-V dual-core RV32IMAFC, 400 MHz). The EVB module here is rev < v3 (`chip_variant: "esp32p4_es"`), which enables `--enable-non-contiguous-regions` on the linker — see pio-idf-p4 SKILL §4.
- **Memory:** 16 MB Flash (QIO @ 80 MHz) / 32 MB PSRAM (HEX @ 200 MHz).
- **Display:** 4.3" **ST7701** RGB panel, 800×480 landscape (`esp_lcd_st7701`). Framebuffer must live in PSRAM. Use the BSP's `bsp_display_start()` + `esp_lvgl_port`; do **not** pull Arduino-Core display libraries.
- **Touch:** **GT911** on the shared I2C bus (`BSP_I2C_SCL=GPIO8`, `BSP_I2C_SDA=GPIO7`); RST/INT not wired (`BSP_LCD_TOUCH_RST = GPIO_NUM_NC`).
- **Connectivity:** WiFi + BLE 5 via a separate ESP32-C6 co-processor over SDIO/SPI (see `managed_components/jc4880_bsp/`).
- **BSP:** pulled as a managed component (`managed_components/jc4880_bsp/`) (forked from `csvke/esp32_p4_jc4880p433c_bsp`); pulls `esp_lcd_st7701`, `esp_lcd_touch_gt911`, `esp_lvgl_port`, `lvgl` from the registry on first build.
- **Upload:** USB-CDC on `/dev/ttyACM0` (P4 exposes USB natively; the JTAG console gotcha is in pio-idf-p4 SKILL §7). Speed 460800.

### Pin assignments

LIN bus on **connector J5 → TX=GPIO27 / RX=GPIO26, UART_NUM_1 @ 9600** (`LIN_TX_PIN`/`LIN_RX_PIN` in `main/main.cpp`). LCD backlight on GPIO23 (`CONFIG_BSP_JC4880P443C_LCD_BL_GPIO=23`). AM2301/DHT22 outdoor sensor DATA on **GPIO52** (`AM2301_DATA_PIN`), read via RMT. Audio I2S codec wired (SCLK=12/MCLK=13/LCLK=10/DOUT=9/DSIN=48/PA=11) but unused.

---

## Display Implementation (`main/ui/p4display.cpp`)

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

> ⚠️ **NVS lives at flash offset `0x9000`. Do NOT flash a full-chip merged
> image to `0x0`** (e.g. `esptool merge-bin … && write-flash 0x0 img.bin`): it
> pads the `0x8000`→`0xe000` gap with `0xFF` and erases NVS, wiping every
> setting below (WiFi, language, MQTT, BLE, tunnel…). Flash by region instead
> (`make flash` / `idf.py flash` never touch `0x9000`). Full details + the
> recovery-free symptom (boots English + no WiFi) in **pio-idf-p4 SKILL §8**.

| Namespace | Keys | Content |
|-----------|------|---------|
| `wifi` | `ssid`, `pass` | WiFi credentials |
| `mqtt` | `host`, `port`, `user`, `pass` | MQTT broker config |
| `display` | `timeout_idx`, `lang` | Screen timeout index, language (0=ES, 1=EN) |
| `solar` | `addr`, `key` | Victron BLE MAC + encryption key |
| `batt` | `addr` | Ultimatron BLE MAC |
| `tank` | `addr` | Tank BTHome sensor MAC (moisture tag 0x2F). Empty = disabled. |
| `multiplus` | `addr`, `key` | VE.Bus dongle MAC + per-device AES-128 bind key. Empty = panel hidden. |
| `openair` | `addr` | OpenAir PLUS A/C (Bergstrom/Dirna) BLE MAC. Empty = CALEFACCIÓN panel; set = CLIMATIZACIÓN panel with cool/eco/heat/off modes. No driver yet (UI-only). |
| `tunnel` | `enabled`, `server`, `token`, `pass` | WSS tunnel config + BasicAuth password for tunneled web access (see wss-tunnel skill) |
| `ota` | `rb_why`, `rb_heap` | Last rollback reason + heap (see firmware-ota skill) |

### Settings screens (⚙ button)

WiFi config · MQTT config (inert, see §MQTT) · Monitorización (Victron/Ultimatron/tank/Multiplus/OpenAir MACs+keys, each with a 🔍 BLE-discovery button; OpenAir adds a pairing flow) · Display (timeout) · Language · Túnel · Actualizaciones.

Entry is `p4SettingsShow()`, called **directly from the ⚙ LVGL event callback** — the port task already holds the LVGL lock there, so no hand-off is needed. Screens that must outlive a callback (BLE/WiFi scans) post an `lv_async_call` populate callback instead. Screens that change credentials reboot on save.

---

## Related skills

- **`pio-idf-p4`** — build system, IDF 6.0 pitfalls, P4 memory layout, ModemManager, corrupted build dir, USB-Serial-JTAG console, CI caching, `PROJECT_VER`, WiFi power save, LittleFS/Flash-button, RMT open-drain, DRAM exhaustion cascade.
- **`esp32p4-docs`** — local RAG over the ESP-IDF docs (`~/esp/esp-idf/docs/en/*.rst`) + all `CONFIG_*` Kconfig options (precomputed `config_index.txt`, `search.sh`). Consult before answering any ESP-IDF/P4 platform, API, cache/memory-tuning or `sdkconfig`-option question — and **always** for complex/cross-subsystem problems, when unsure an option exists, or when a fix is proving resistant, so no Kconfig knob (like the L2 cache) is missed.
- **`firmware-ota`** — P4 self-OTA: GitHub-Releases-direct discovery, versioning, auto-prompt policy, transfer tuning, PENDING_VERIFY self-test/rollback net.
- **`wss-tunnel`** — WSS reverse tunnel (firmware side): two-httpd split, Nagle on loopback, LRU eviction, mbedtls/PSA heap, plus the local httpd/WS server gotchas. Bridge side: companion repo [`vidorado/truminus-cloud-server`](https://github.com/vidorado/truminus-cloud-server) → `tunnel-bridge` skill.
- **`ui-interfaces`** — LCD touch UI ↔ WebSocket web UI coordination, layout, responsive CSS gotchas, panel semantics.
- **`lvgl-fonts`** — Tiny TTF font loading, FA6 icon subset, `gen_icon_font.py`, EEZ Studio.
- **`truma-protocol`** — full Truma LIN frame reference (master/slave frames, byte layouts).
- **`victronble`** — Victron Instant Readout BLE (Solar / SmartShunt / BMV).
- **`multiplusble`** — Victron VE.Bus / Multiplus Instant Readout (record 0x0C).
- **`ultimatronble`** — Ultimatron BMS GATT protocol.
- **`openair-plus`** — OpenAir PLUS (Bergstrom/Dirna) A/C BLE protocol: telemetry frame, command writes, pairing handshake.
