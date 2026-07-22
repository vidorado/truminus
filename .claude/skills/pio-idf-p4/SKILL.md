# Skill: ESP-IDF 6.0 + ESP32-P4 (board jc4880_p4)

**Role:** Specialist on the native `idf.py` build for ESP32-P4 with ESP-IDF 6.0.1.
**Goal:** Diagnose build/link failures without repeating known detours.

> **Not sure, or stuck?** This skill records what we already learned. For
> anything it does not fully cover — an unfamiliar `CONFIG_*`, a resistant
> symptom, a complex memory/cache/PSRAM/SDIO/TLS trade-off — consult the
> **`esp32p4-docs`** RAG (local ESP-IDF docs + every Kconfig option) before
> concluding, so nothing is missed.

> PlatformIO was removed (2026-05-18). The build is now driven entirely by
> `idf.py` via the repo `Makefile`. There is no `platformio.ini`, no
> `platformio_idf.py`, no `.pio/` build cache. The flashable binary is
> `build/truminus.bin` produced by `idf.py build`.

---

## 1. Build commands (quick reference)

```bash
# Once per machine
git clone --branch release/v6.0 https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf && ./install.sh esp32p4

# Once per terminal session
. ~/esp/esp-idf/export.sh

# Build / flash / monitor
make               # idf.py build
make flash         # idf.py -p /dev/ttyACM0 flash   (PORT=... to override)
make monitor
make flash-monitor
make clean         # idf.py fullclean
```

---

## 2. Corrupted `build/` directory

If cmake fails with `cannot read spec file '…/build/specs/picolibc.specs'`,
`build/build.ninja` is missing, or any cmake configure error: **delete `build/`
entirely** and rebuild from scratch. cmake left a partial state that it cannot
recover from incrementally.

```bash
rm -rf build/
make build
```

First build compiles all of IDF from scratch — takes several minutes.

---

## 3. sdkconfig management

There is now a single `sdkconfig` (project root), used by `idf.py`. The
`sdkconfig.jc4880_p4` file (used by PlatformIO's cmake) no longer exists.

Project-specific overrides are in `sdkconfig.defaults`:
- `CONFIG_HTTPD_WS_SUPPORT=y` — required for the WebSocket server.
- Extended-advertising **reception** needs three NimBLE knobs, not just
  `CONFIG_BT_NIMBLE_EXT_ADV=y` (that only compiles the feature):
  `CONFIG_BT_NIMBLE_EXT_SCAN=y` (actually scan for ext adverts) **and**
  `CONFIG_BT_NIMBLE_TRANSPORT_EVT_SIZE=257` (the legacy 70 B truncates ext-adv
  HCI reports over the C6 VHCI → they're dropped). The build prints an `info:`
  warning when these are stale vs Kconfig. See §15.

If you need to change `sdkconfig`: run `idf.py menuconfig`, or edit
`sdkconfig.defaults` for options that should be preserved across `fullclean`.

---

## 4. ESP32-P4 rev < v3 memory — non-contiguous SRAM

The board JSON (`boards/jc4880_p4.json`) declares `chip_variant: "esp32p4_es"`
→ enables `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` → linker uses
`-Wl,--enable-non-contiguous-regions`.

**SRAM layout** (from `esp-idf/esp_system/ld/memory.ld`):
- `sram_low  (RWX) @ 0x4FF00000` — ~179 KB — shared IRAM/DRAM
- `sram_high (RW)  @ 0x4FF40000` — ~256 KB — DRAM
- PSRAM (irom/drom) @ 0x48000020 — 64 MB

With `--enable-non-contiguous-regions`, when a section does not fit in
`sram_low` the linker **silently discards it** instead of overflowing.
Discarded sections cause cascading `unresolvable R_RISCV_*` errors.

**The cause is always a config option inflating IRAM.** Usual suspects:
`FREERTOS_IN_IRAM=y`, `LIBC_NEWLIB=y`, `CXX_EXCEPTIONS=y`. Fix in
`sdkconfig.defaults`, not in `memory.ld`.

---

## 5. mbedtls 4.x / tf-psa-crypto (IDF 6.0)

IDF 6.0 ships mbedtls 4.x with a `tf-psa-crypto` component. The final `.a`
that gets linked is `build/esp-idf/mbedtls/mbedtls/tf-psa-crypto/core/libtfpsacrypto.a`.
Symbols `mbedtls_mutex_lock`, `mbedtls_calloc`, `mbedtls_platform_zeroize`
live inside that archive. If they appear as undefined (`U`), the archive is
stale — `rm -rf build/` and rebuild.

---

## 6. DRAM exhaustion cascade — `MBEDTLS_ERR_SSL_ALLOC_FAILED` ↔ SDIO assert

Symptom pair seen together (always together, ~1 s apart):

```
E (...) esp-tls-mbedtls: mbedtls_ssl_handshake returned -0x3B00
E (...) esp-tls: Failed to open new connection
...
assert failed: sdio_rx_get_buffer sdio_drv.c:953 (*buf)
```

Both are symptoms of the **same root cause**: internal DRAM exhausted while
TLS handshakes are in flight. `-0x3B00 = MBEDTLS_ERR_SSL_ALLOC_FAILED` is
`malloc() == NULL` inside mbedtls. The SDIO RX path needs a DMA-capable
(internal-DRAM) buffer for the next inbound packet from the C6 and
similarly gets NULL → `assert` → reboot loop. The trigger is almost always
the WSS reverse tunnel coming up right after `IP_EVENT_STA_GOT_IP`: TLS
handshake state and the 16 KB `SSL_IN_CONTENT_LEN` buffer land in internal
DRAM unless steered to PSRAM.

The three knobs in `sdkconfig.defaults` (pinned with a comment header):

| key | from → to | why |
|---|---|---|
| `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC` | `y` → `n` | stop pinning all mbedtls allocs to DRAM |
| `CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC`  | (off) → `y` | use the global heap, which spills to PSRAM |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` | 16384 → 2048 | lower the threshold so >2 KB allocs land in PSRAM |
| `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN`   | kept 16384 | the GitHub release CDN sends full-size TLS records; 4 KB broke self-OTA (mbedtls -0x0087 "Complete headers were not received"). With the alloc steered to PSRAM (rows above) the 16 KB buffer is no longer dead DRAM. |

**The decisive lever (1.4.12): halve the L2 cache.** The knobs above steer *heap*
to PSRAM but the steady-state internal floor stayed ~24 KB, so a busy board still
tripped this assert (and the OTA self-test rollback). The P4 **L2 cache is carved
from the same HP L2MEM as the heap**: `CONFIG_CACHE_L2_CACHE_128KB` (was 256)
returns **~128 KB of internal, DMA-capable SRAM** — steady-state free internal
DRAM **~24 KB → ~117 KB**. Sizes are 128/256/512 only (no middle ground). This is
the lever the "immovable ~25 KB floor" analysis below missed.

**Coupled config — code out of PSRAM.** A smaller L2 = more code cache misses =
more PSRAM reads; with `SPIRAM_XIP_FROM_PSRAM` those contend with the LCD
framebuffer DMA (also PSRAM) → **panel glitches**. Fix:
`# CONFIG_SPIRAM_XIP_FROM_PSRAM is not set` runs `.text`/`.rodata` from flash (the
separate flash MSPI bus). **DRAM-neutral** (code → flash, not internal DRAM), so
it keeps the L2 headroom; verified glitch-free in steady-state, UI smooth.
**Ship the two together** (`sdkconfig.defaults`).

**Caveat — this fixes steady-state, NOT the OTA *download*, and that one is
UNFIXABLE here (accepted as cosmetic).** During a self-OTA the app *writes*
~2.3 MB to the app1 flash partition, and the whole download shows a black↔cyan
flicker on the full-screen OTA progress screen (cosmetic; content stays readable,
the update is 100% reliable). Root cause was pinned on the bench with a `glitch`
CLI diagnostic (stress one bus at a time behind the OTA screen — not committed):

- **It is NOT flash-bus vs framebuffer, and NOT PSRAM-bus contention.** Hammering
  PSRAM↔PSRAM memcpy at ~51 MB/s (above the framebuffer's ~46 MB/s) did **not**
  flicker. Only flash *writes* did.
- **It is the cache disable.** Every flash program/erase calls
  `spi_flash_disable_interrupts_caches_and_other_cpu()`, which turns off the **L2
  cache**. On the P4 *all* PSRAM access — including the MIPI-DSI DMA fetching the
  framebuffer — is routed through L2, so while the cache is off the scan-out
  starves → underrun → cyan. Both **erase** (long ~40 ms cache-off, worse) and
  **program** (sub-ms, milder) windows flicker.

Why nothing clean fixes it on this board:

- **Bounce buffer** (the textbook fix — feed the panel from an internal-SRAM slice
  immune to cache-off) **does not exist for MIPI-DSI** in IDF 6.0 *or master*
  (`esp_lcd_dpi_panel_config_t` has no `bounce_buffer_size_px`; it is RGB-only).
- **Framebuffer in internal RAM** is impossible: 800×480×2 = **768 KB** ≫ the
  ~117 KB free internal DRAM.
- **`CONFIG_SPI_FLASH_AUTO_SUSPEND`** *would* fix it (keeps the cache enabled,
  suspending the flash op only briefly for reads) — but on this board's **Boya**
  flash (mfr `0x68`; IDF suspend is validated only for GD/Winbond/XMC/ISSI) it
  **boot-loops the board** (early hang at flash init / XIP fetch, register dump).
  Verified on the bench — **do NOT re-enable it.**

So the download flicker is architectural (P4 + MIPI-DSI + PSRAM framebuffer +
cache-off-on-flash-write) and is **accepted as cosmetic**. `bulk_flash_erase=true`
was considered but rejected: it only moves the *erase* flicker into one upfront
burst; the *program* flicker persists through the whole download, so the visible
result is unchanged. A harmless side-cleanup shipped anyway: `displaySyncTick()`
skips the whole-UI rebuild while `p4OtaInstalling()` (the main screen is inactive
under the OTA screen), saving pointless CPU/LVGL work — it does **not** affect the
flicker (that is below LVGL).

**Diagnosing future cases:**
- Print free internal heap (`heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`)
  in the wstunnel task right before `esp_websocket_client_start()` and
  inside the WS event handler — sub-20 KB at handshake time = trouble
  brewing.
- `idf.py size-components --diff` after the config change should show
  mbedtls and `lwip` shifting from `.dram` to `.ext_ram`.
- Don't "fix" by disabling the tunnel. The tunnel is just the trigger;
  any large enough TLS workload (OTA, HTTPS client, MQTT-TLS) will hit
  the same heap and crash the C6 link.

---

## 7. `/dev/ttyACM0` is USB-Serial-JTAG, not UART0

The P4 exposes USB-Serial-JTAG natively on the USB-C port (no CH340 /
CP210x). UART0 lives on header pins (not wired on jc4880_p4). Knock-on
effects:

- **Console primary must be `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`**
  (set in `sdkconfig.defaults`). If primary is `ESP_CONSOLE_UART_DEFAULT`,
  `ESP_LOGx` output is duplicated to JTAG only because secondary is
  enabled, but `stdin` reads come from UART0 — which the user is not
  connected to. Symptom: a picocom user sees logs and (with the right
  REPL) sees the prompt, but typing produces zero echo and zero command
  effect. Fix is the sdkconfig change, not picocom flags.

- **`esp_console` REPLs must use `esp_console_new_repl_usb_serial_jtag()`**
  (see `main/cli.cpp::cliStart`). The UART variant binds the line editor
  to UART0 and the input vanishes into the void. The JTAG variant only
  declares its config struct if primary console is JTAG (the `#if
  CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` in `components/console/esp_console.h`),
  so the previous point and this one are linked.

- **linenoise must run in dumb mode on USB-Serial-JTAG.** The JTAG
  endpoint doesn't reply to linenoise's `ESC[6n` cursor probe in the
  expected window, so the probe fails and linenoise falls back to a
  half-broken state where arrow keys leak as raw `[D[C[A[B` and
  backspace echo is unreliable. `linenoiseSetDumbMode(1)` is required;
  it trades arrow editing + history for a working char-by-char echo and
  BACKSPACE. Ctrl+U still clears the line, which is the practical
  recovery for typos. Don't try to "fix" the probe — it's a hardware
  limit of how USB-Serial-JTAG schedules CDC reads.

---

## 8. Upload / OTA

```bash
# USB-CDC (default)
make flash PORT=/dev/ttyACM0

# OTA (device must be running and on the network)
idf.py -p truminus.local app-flash
```

ModemManager on Linux grabs `/dev/ttyACM0` on first plug. If flash fails with
"port is busy": `sudo systemctl stop ModemManager`.

> ⚠️ **On an OTA'd board, `app-flash` writes the partition that is NOT running.**
> After a self-OTA the device boots from **app1** (`0x410000`); `idf.py app-flash`
> writes **app0** (`0x10000`) and does **not** touch otadata, so the flashed image
> **never boots** — the board keeps running the OTA'd image and your changes seem
> to have no effect (bench symptom: edits/log probes silently absent, cadence
> unchanged). Use the full **`idf.py flash`** (it writes `ota_data_initial.bin` at
> `0xe000`, resetting the selection back to app0) when USB-flashing over an OTA'd
> board. `app-flash` is only safe when the board is already running app0.

**NEVER flash a full-chip merged image to `0x0` on a configured board — it
ERASES NVS.** `esptool merge-bin -o img.bin 0x2000 … 0x10000 truminus.bin
0x810000 littlefs.bin` pads every gap with `0xFF`, and **NVS lives at `0x9000`**
(in the gap between the partition table at `0x8000` and otadata at `0xe000`, see
`partitions_16MB.csv`). `write_flash 0x0 img.bin` therefore writes `0xFF` over
NVS and wipes WiFi creds, language, MQTT, BLE MACs/keys, the tunnel
server/token/password — everything. Symptom after such a flash: the board boots
in **English** (the i18n in-memory default is `EN`) and with **no WiFi** — that
is a wiped NVS, not a firmware bug. (Learned the hard way, 2026-06.)

To flash while **preserving** NVS, write by region — never the `0x0` blob:

```bash
make flash PORT=/dev/ttyACM0          # idf.py flash: bootloader+ptable+otadata+app+littlefs, NOT nvs
# or explicit (note: no 0x9000 entry):
python -m esptool --chip esp32p4 -p /dev/ttyACM0 write-flash \
  0x2000 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin \
  0xe000 build/ota_data_initial.bin 0x10000 build/truminus.bin 0x810000 build/littlefs.bin
```

The merged-`0x0` image is **only** for a deliberate factory reset or a
brand-new board. If you must distribute a single-file image, flash it to a
fresh board, or accept it resets settings.

---

## 7. ESP-Hosted BT controller — required init sequence

The C6 slave firmware does **NOT** auto-initialize BT at boot. The host must
call the following RPC functions via ESP-Hosted before starting NimBLE:

```cpp
extern "C" {
#include "esp_hosted_misc.h"   // must be inside extern "C" — header has no guards
}
// After wifi_manager_start(), before victronBleInit() / bleSupervisorStart():
ESP_ERROR_CHECK(esp_hosted_bt_controller_init());
ESP_ERROR_CHECK(esp_hosted_bt_controller_enable());
```

**Symptom without this:** `E (xxx) NimBLE: HCI wait for ack returned 19` — NimBLE
hangs on HCI Reset because the C6 slave never brought up the BT controller.

**CMakeLists.txt:** add `espressif__esp_hosted` to the component's `REQUIRES` list
(it is a managed component, not part of the core IDF SDK).

**`extern "C"` guard:** `esp_hosted_misc.h` lacks `extern "C"` guards. Without the
wrapper the C++ compiler mangles the function name and the linker emits
`undefined reference to esp_hosted_bt_controller_init()` even when the library
is in REQUIRES.

---

## 8. VSCode integration

Install `espressif.esp-idf-extension`. After the first `make build`, the file
`build/compile_commands.json` is generated and IntelliSense picks it up.

Copy `.vscode/settings.json.template` → `.vscode/settings.json` and fill in
your IDF path. `settings.json` is gitignored.

---

## 9. CI build caching (`.github/workflows/{ci,release}.yml`)

`release.yml` builds + attaches `truminus.bin` on every `X.Y.Z` tag;
`ci.yml` builds on every push to `master`. Getting ccache to actually hit
took fixing **three** independent traps — symptom each time was "cache
restored" but **0 hits** and a full ~5-6 min build. Use `ccache -p`
(effective config) and `ccache -s -v` (per-reason stats) to diagnose, not
guesswork.

1. **Mount path.** `espressif/esp-idf-ci-action` mounts the repo at
   `/app/<owner>/<repo>`, **not** `/github/workspace`. So `CCACHE_DIR` must
   be `$PWD/.ccache` (the mounted dir, which `actions/cache path: .ccache`
   persists). A host-side `${{ github.workspace }}` path silently saves
   nothing ("Path Validation Error").
2. **`env:` is not forwarded into the container.** Only what you `export`
   inside the action's `command` reaches ccache. `CCACHE_*` set via the step
   `env:` block show up as **defaults** in `ccache -p` (`compiler_check =
   mtime`). Export every setting in `command`.
3. **Fresh container = new mtimes.** The IDF image is extracted per run, so
   the compiler + every header get new mtimes; the default `compiler_check =
   mtime` then misses everything. Need `CCACHE_COMPILERCHECK=content` plus
   `CCACHE_SLOPPINESS=time_macros,locale,include_file_ctime,include_file_mtime`.

Plus the **cache-scope** rule: Actions caches are scoped per ref, and tag
runs are mutually isolated — a tag can only restore from its own ref or the
**default branch**. So the `master` build (`ci.yml`) is what populates the
shared cache; release (tag) runs restore it via the common `ccache-esp32p4-`
key prefix (per-commit key + `restore-keys` fallback). Without a master
build, every release is cold. `concurrency: cancel-in-progress` stops rapid
master pushes from stacking. Result: releases ~2.5 min (now dominated by
container pull + `cmake configure` ~40 s + littlefs + link, not compilation).

The canonical build command (both workflows):

```yaml
command: >-
  export CCACHE_DIR="$PWD/.ccache" CCACHE_BASEDIR="$PWD"
  CCACHE_COMPILERCHECK=content CCACHE_NOHASHDIR=true
  CCACHE_SLOPPINESS=time_macros,locale,include_file_ctime,include_file_mtime
  CCACHE_MAXSIZE=500M IDF_CCACHE_ENABLE=1
  && pip install littlefs-python && idf.py build && ccache -s
```

## 10. `PROJECT_VER` is captured at *configure* time

`git describe --tags` runs in the root `CMakeLists.txt` before `project()`.
cmake does **not** reconfigure just because git state changed, so an
incremental build/flash after a new commit or tag keeps baking the **stale**
version (we once flashed `1.1.0-dirty` while the tag was `1.1.1`, and a
locally-built `truminus.bin` carried the old `esp_app_desc_t.version`). Fix:
the root `CMakeLists.txt` lists `.git/logs/HEAD` + `.git/packed-refs` in
`CMAKE_CONFIGURE_DEPENDS` so a commit/checkout forces a reconfigure. It's
reconfigure-only (seconds; ninja still incremental). `.git/index` is not
watched (would trigger on every `git add`). Verify with the
`-- TruMinus: firmware version = …` line and `strings build/truminus.bin | grep`.

## 11. Multiple Espressif boards on USB / WSL

When more than one Espressif chip is attached (e.g. a stray ESP32-C3 dev
board alongside the P4), `/dev/ttyACM{0,1,…}` ordering is not stable and a
flash can target the wrong chip (`A fatal error occurred: This chip is
ESP32-C3, not ESP32-P4`). Identify the P4 before flashing:

```bash
for p in /dev/ttyACM*; do echo -n "$p -> "; \
  python -m esptool --port $p --chip auto chip-id 2>&1 | grep -m1 "Detecting chip type"; done
```

Then `idf.py -p /dev/ttyACMx flash`. On WSL the P4 must be attached via
`usbipd attach --wsl --busid <id>` (PowerShell); a DTR/RTS reset from a
failed flash can re-enumerate it. The P4's USB-Serial-JTAG re-enumerates on
every reset, so a serial monitor drops on each OTA/rollback reboot (and
holding the port from a monitor makes `esptool`/flash report the port busy).

## 12. WiFi power save throttles *everything* — disable it

The default `WIFI_PS_MIN_MODEM` sleeps the station between DTIM beacons and
adds ~100 ms latency to every receive cycle. Symptoms: OTA downloads crawl at
~30 KB/s, the web UI feels janky, the WSS tunnel is laggy — all the same root
cause. This is a mains-powered controller, so `wifi_manager.cpp` sets the PS
mode right after `esp_wifi_start()` (proxied to the C6 via `esp_wifi_remote`).

**BLE coexistence — `MIN_MODEM` makes BLE *worse*, not better (measured
2026-06):** intuition says modem sleep frees airtime for the shared BLE radio,
so it should help reception. It does the opposite here — a phone advert visible
at point-blank under `PS_NONE` vanished entirely under `MIN_MODEM`. On this
C6/esp_hosted shared front end, modem sleep disrupts the BLE scan windows. So
`PS_NONE` wins for **both** WiFi throughput and BLE; keep it. The BLE-reception
problem is sensitivity/antenna (§15), not power-save tuning — don't reach for
`MIN_MODEM` here.

## 13. LittleFS image rebuilds every cmake run; VSCode Flash button bypasses our args

- `littlefs_create_partition_image()` rebuilds the bin on every cmake run —
  CMake can't watch directory contents, so the helper uses
  `add_custom_target ... ALL` with no input deps; the bin gets a fresh mtime
  even when `data/` is identical. Mitigated by `--skip-flashed` (esptool
  MD5-compare; baked into every flash target's `SUB_ARGS` in the root
  `CMakeLists.txt`). Inputs are deterministic (`littlefs-python` + `mtime=0`
  in `gen_gz.py`) so the comparison succeeds.
- The VSCode IDF extension's **Flash button** hardcodes esptool args in
  TypeScript, ignores `write_flash_args` in `flasher_args.json`, and offers no
  setting for extra args — so it bypasses `--skip-flashed`. Work around with
  `.vscode/tasks.json` tasks ("TruMinus: Flash"/"Flash + Monitor") that invoke
  `idf.py flash` (which *does* honour our args); `VsCodeTaskButtons.tasks` in
  `.vscode/settings.json` puts status-bar buttons next to them
  (`spencerwmiles.vscode-task-buttons`). The tasks must source
  `$IDF_PATH/export.sh` first because `idf.py` is not on PATH in a fresh shell.
- This image also ships **over OTA**: a content hash (`data/fs.ver`, by
  `gen_fs_ver.py`) is baked in and published as the `littlefs.ver` release
  asset so the device resyncs the web only when it changed. That half lives in
  the **firmware-ota** skill ("Web-asset (LittleFS) sync").

## 14. RMT on an open-drain single-wire bus must idle *high*

The single-wire AM2301/DHT22 reader (`main/am2301.cpp`, DATA on GPIO52) uses an
RMT TX channel (start pulse) + RX channel (response) bound to the **same GPIO**
(IDF 6.0 wires them in loopback automatically — no `io_loop_back` flag, and
`io_od_mode` is gone too: call `gpio_od_enable()` after channel creation). The
trap: a TX channel's idle/end-of-transmission level defaults to **0**, and on an
open-drain line that is an *active* pull-low — so after the start pulse the bus
stays at 0 V, the sensor never sees the release and never responds (symptom:
`rx timeout`). Fix: `tx_cfg.flags.init_level = 1` **and**
`rmt_transmit_config_t.flags.eot_level = 1` so the line idles released (high)
through the pull-up. Also: `mem_block_symbols` must respect
`SOC_RMT_MEM_WORDS_PER_CHANNEL` (**48** on the P4), and `signal_range_max_ns`
must exceed your own start-low pulse (RX is armed first and captures it) while
still ending the frame on the indefinite trailing high. The first read after
power-up routinely fails while the sensor settles — retry, don't treat it as an
error.

## 15. BLE reception on the C6 — sensitivity, ext-adv, `scan` tool

BLE runs on the **ESP32-C6 co-processor** over SDIO (the P4 has no radio); it
time-shares one RF front end with WiFi. Field symptom: weak/flaky reception of
the Victron gear (SmartSolar, Multiplus, Ultimatron) — seen, but at the edge.

**Reception is sensitivity-limited — it's the antenna.** Clean cross-check
(2026-06): put a phone (nRF Connect *Scanner*) where the board sits and compare
RSSI on the **same far devices**. Measured: far devices read **~12-18 dB weaker
on the board** than on the phone (e.g. a beacon at −68 phone / −84 board;
another −60 / −72). ~16 dB ≈ ~6× range — exactly the "seen but very weak at 3 m
through wood" symptom. A *very close* device showed almost no gap (−60 phone /
−63 board): **near-field (<~20 cm ≈ 1.6λ) masks the antenna deficit**, so
point-blank comparisons lie — don't use them. No firmware knob recovers this
(scan window, power-save, ext-scan are all correctness/coexistence, not range).
Also don't compare RSSI/advert-rate across *different* devices (each has its own
TX power + advertising interval); only the **same** device across a change is
comparable. **Fix is the C6 antenna** — the BSP documents none; check the board
for a u.FL/IPEX connector or an antenna-select jumper near the C6 (a board
shipped on an unconnected external-antenna path explains the stock deficit).
WiFi power save is **not** a lever — `MIN_MODEM` made BLE worse (§12).

**Extended advertising was a real gap (fixed 2026-06).** The Multiplus VE.Bus
dongle advertises with BLE-5 *extended* advertising; SmartSolar/Ultimatron are
legacy/connectable. A legacy-only scan silently misses ext adverts — see §3 for
the three sdkconfig knobs. Signature of the bug (or of a too-weak link): the
device shows as a **nameless-MAC ghost** — the primary `ADV_EXT_IND` (MAC+RSSI)
is caught but the `AUX_ADV_IND` carrying the name/payload is dropped. Verified
end-to-end: a non-connectable extended advert from a phone is invisible at
range, nameless-MAC at the margin, full name when touching the board.

**Diagnostic tool — serial CLI `scan [secs]`** (`main/cli.cpp`): lists every
advertiser with RSSI + **ADV** (adverts heard this window = reception-quality
metric). It disables the controller's duplicate filter for the scan window
(restored after) so ADV reflects the true received rate. Reuses
`bleDiscoveryScan(false, …)` (victron_only=false returns **all** devices).
Pair it with a phone running a fixed advertiser (nRF Connect → Advertiser) at a
fixed marginal distance as a stable reference for A/B-ing reception tweaks.

---

## 16. Internal-DRAM budget — diagnosing & reclaiming (esp_hosted board)

This board is **internal-DRAM-starved**, not flash- or PSRAM-starved. PSRAM
(32 MB) sits nearly empty; the scarce pool is internal SRAM. Almost every
memory crash on this board traces back here.

> **Verify before advising — this section is a snapshot, not live truth.** The
> figures below are point-in-time and drift as config changes (the L2-cache
> reclaim already moved DIRAM total from ~435 KB to ~576 KB, so the numbers
> below understate current headroom). Before giving *any* memory-tuning
> recommendation, unconditionally:
> 1. Run `idf.py size` for current numbers.
> 2. **Read IDF's canonical lever list — `docs/en/api-guides/performance/ram-usage.rst`
>    (open it via the `esp32p4-docs` skill).** It is the authoritative menu
>    (Reducing Static/Stack/Heap/IRAM usage, `Determining Stack Size`); this
>    section only records the board-specific extras layered on top, so start
>    from the guide, not from here.
> 3. For every `CONFIG_` you cite or weigh, confirm its current
>    default/constraint in `esp32p4-docs` (`search.sh <name>` / `config_index.txt`)
>    — cache size, PSRAM mode, stack-in-PSRAM and brownout level all have P4 /
>    silicon-revision caveats that live only in Kconfig and are easy to get wrong
>    from memory.
>
> Do not conclude from this section's numbers alone.

**The budget (measured 2026-06):**
- DIRAM total ~435 KB; static firmware ~125 KB (`.text`-in-IRAM 85 KB + `.bss`
  24 KB + `.data` 18 KB) → ~310 KB heap at boot (`idf.py size`).
- Runtime fills it to **~22 KB free steady-state** (esp_hosted WiFi+BLE over
  SDIO + NimBLE host + lwip + the WSS tunnel's TLS). PSRAM stays ~26 MB free.
- Internal heap is ~95 % full: a 256 KB region of ~2100 small (~124 B) blocks +
  a 113 KB region — network/TLS/BLE working set, much of it DMA-pinned.

**Why it bites — the OTA self-test heap floor.** `p4_ota.cpp` rolls back if free
internal stays below `HEAP_FLOOR` (12 KB) for `HEAP_BREACH_LIMIT` samples. With
only ~10 KB headroom, anything that spikes internal use during the
PENDING_VERIFY boot tips it over. Observed trigger: **weak BLE coverage** — a
failing/slow Ultimatron GATT connect holds NimBLE buffers longer, and (with BLE
bring-up overlapping WiFi's transient buffers) free internal dipped to **6 KB**
→ "heap floor breached" rollback. The same starvation also surfaced as a NimBLE
`Load access fault` in `NimBLEAdvertisedDevice::update` (corruption near OOM).
The rollback reason is written to the faultlog `diag` record so it shows in the
web About overlay even on the rolled-back-to image (see firmware-ota skill).

**Forced to internal — cannot move to PSRAM:**
- Task stacks from plain `xTaskCreate`. `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`
  only *permits* PSRAM stacks via `xTaskCreateWithCaps(MALLOC_CAP_SPIRAM)`, and
  only for tasks that never run while the flash cache is disabled (no flash/OTA,
  not in ISR) — risky.
- esp-aes scratch, the LCD framebuffer DMA (`MALLOC_CAP_DMA`).
- Allocations below `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (2048) — internal by
  the threshold. Many of the ~2100 small blocks are here.

**Movable to PSRAM in principle — but DON'T, see why (2026-06 audit):** the
earlier draft of this section listed the esp_hosted SDIO DMA pool and the NimBLE
host pools as irreducible. **That was wrong** — both move, the question is
whether it's safe and whether it helps the metric you care about:
- **esp_hosted SDIO RX/TX pool (~90 KB, the single biggest internal hog).**
  `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y` steers it to DMA-capable PSRAM
  (the P4 GDMA reaches PSRAM through cache). Measured: **+72 KB steady-state free
  internal** (24 KB → 96 KB). BUT the SDIO **TX** DMA reading from PSRAM is
  **unstable on this board** — at *idle* (not even during a flash write) the
  write task hit `ESP_ERR_TIMEOUT (258)` → "Unrecoverable host sdio state" →
  hosted transport self-restart, intermittently after tens of seconds. The
  single Kconfig switch steers RX and TX together (no RX-only option), so the
  TX instability rules it out. **Left internal, pinned off in `sdkconfig.defaults`
  with a warning.** This is the reason these DMA buffers stay internal — not that
  they "can't" move.
- **NimBLE host pools (msys/ACL/EVT): ~16 KB internal** (measured cleanly; an
  earlier contaminated reading said ~0). `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`
  moves them to PSRAM (`esp_nimble_mem.c` maps to `MALLOC_CAP_SPIRAM`). Untested
  here — controller is on the C6 so the HCI mbufs ride the hosted transport, not
  local DMA, so it's plausibly safe — but unnecessary (see floor note below).

**The steady-floor self-leveling trap (key finding).** Freeing a *non-fixed*
internal allocation (e.g. moving the 12 KB WS queue to PSRAM via
`xQueueCreateWithCaps`) does **not** raise the steady-state free-internal floor:
the lwip/TLS/hosted working set is demand-driven and self-levels to consume
whatever you freed before steady state (proof: at steady, `free ≈
largest_free_block` — internal is packed into one ~24 KB island regardless).
Only relocating a **fixed, pre-allocated** chunk the working set cannot reclaim
(the SDIO mempool) actually raises the floor — and that's exactly the one that
destabilises the transport. Net: the steady floor (~25 KB, min ~25 KB over a
3.4 h soak, no leak) is effectively immovable here by safe means. What *is*
movable and worth doing is the **bring-up transient** (see levers below).

**Diagnostic toolkit:**
- Runtime split: `heap_caps_get_free_size / _minimum_free_size /
  _largest_free_block` for `MALLOC_CAP_INTERNAL` vs `MALLOC_CAP_SPIRAM`;
  `heap_caps_print_heap_info(MALLOC_CAP_INTERNAL)` for per-region free/blocks.
- Static: `idf.py size` (DIRAM used/remain), `idf.py size-components`.
- Per-task stack high-water: `uxTaskGetSystemState()` (needs
  `CONFIG_FREERTOS_USE_TRACE_FACILITY=y`, already on) → `usStackHighWaterMark` =
  min free stack ever; a large value = the stack is over-allocated, shrink it.
- Per-task heap ownership: `CONFIG_HEAP_TASK_TRACKING=y` +
  `heap_caps_get_per_task_info()`.
  - **GOTCHA (hit 2026-06):** tracking stores per-block metadata *in internal
    heap*; with ~2000+ blocks the overhead (tens of KB) itself exhausts an
    already-starved internal heap → `assert failed: vApplicationGetIdleTaskMemory
    port_common.c (pxStackBufferTemp != NULL)` (the idle/timer task stack alloc
    returns NULL at scheduler start, before your diag code even runs). On a tight
    board you can't measure with tracking — reason from `print_heap_info` +
    stack high-water instead, or free a big chunk first.
- Caller-level **heap tracing** (`CONFIG_HEAP_TRACING_STANDALONE`, record buffer
  placeable in PSRAM so it doesn't starve internal) is the natural alternative —
  **but on RISC-V it's effectively blocked here**: `HEAP_TRACING_STACK_DEPTH` is
  clamped to 0 (no `alloced_by[]`) unless `CONFIG_ESP_SYSTEM_USE_FRAME_POINTER=y`,
  and enabling frame pointers grows the **bootloader past its 0x6000 limit**
  (`Bootloader binary size too large for partition table offset 0x8000`) unless
  you also move `CONFIG_PARTITION_TABLE_OFFSET` (invasive — shifts the flash
  layout). Net: per-allocation attribution of the internal working set isn't
  practical on this target without partition surgery. **But you don't need it:**
  the cheap, build-real alternative is **differential measurement** — snapshot
  `heap_caps_get_free_size(MALLOC_CAP_INTERNAL/SPIRAM)` at each subsystem
  bring-up boundary and read who consumes what from the deltas (see
  `main/heapdiag.cpp`, gated by `HEAP_DIAG` in `flags.h`, off by default).
  GOTCHA hit in the 2026-06 second audit: an async task (the BLE supervisor)
  whose marks interleave with `bootTask`'s contaminates the deltas via the shared
  `prev` — serialize bring-up (start BLE *last*) so the sequential marks are
  clean, or you'll misattribute (it briefly made NimBLE-init look like ~0 KB
  when it's really ~16 KB).
  Conclusion from the **second 2026-06 audit** (corrects the first): the internal
  hog is the **esp_hosted SDIO DMA pool (~90 KB)** + lwip/TLS working set. The
  big pool *is* movable to PSRAM (`MEMPOOL_PREFER_SPIRAM`) but that destabilises
  the SDIO transport (see above), so in practice it stays internal. This audit
  concluded the steady-state floor (~25 KB) was "immovable by safe means" — that
  was **superseded in 1.4.12**: halving the L2 cache (§6) returned ~128 KB of
  internal SRAM, lifting the floor to ~117 KB. The bring-up-transient wins below
  still stand, but are no longer the *only* lever.
  **Validated in the field (1.2.15):** the BLE-after-WiFi settle kept the
  post-OTA self-test minimum free internal at ~23.7 KB (vs 6 KB / rollback in
  1.2.13). The 2026-06 follow-up added two more safe transient wins — moving the
  12 KB WS queue to PSRAM (`xQueueCreateWithCaps`) and creating the BLE-start
  task *after* `boot:complete` — which lifted the worst bring-up dip from ~6 KB
  to ~84 KB. None of these raise the steady floor; they de-risk the verify-boot.

**Reclaim levers, ranked (safe → risky):**
1. **Trim NimBLE buffer counts** — defaults assume high-throughput BLE; here it's
   passive scan + one periodic GATT poll. Cut `BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT`,
   `_EVT_COUNT`, `MSYS_1/2_BLOCK_COUNT`, `MAX_CONNECTIONS` (3→2: only the battery
   connects; keep 1 spare for teardown overlap). **Keep `TRANSPORT_EVT_SIZE=257`**
   — ext-adv (Multiplus) reports need it. ~15 KB, safe.
2. **Shrink over-allocated task stacks** using the high-water data. Safe.
3. **Do NOT cut `LWIP_MAX_SOCKETS` (40) / `LWIP_MAX_ACTIVE_TCP` (64)** — raised
   deliberately for the tunnel (a multi-asset page opens many fds/PCBs); cutting
   them reintroduces `accept errno 113` / socket exhaustion.
4. **Sequence bring-up, don't stack peaks** — start NimBLE *after* WiFi has
   associated and its transient bring-up buffers settle (`bleSupervisorTask`
   warmup waits for WiFi + settle), so BLE+WiFi peaks don't coincide during the
   heap-tight verify boot. NB: do **not** special-case PENDING_VERIFY to skip the
   GATT connect — the self-test must exercise that path to catch a panic in it.
5. **Move fixed non-DMA buffers to PSRAM** — e.g. the 12 KB WS queue via
   `xQueueCreateWithCaps(…, MALLOC_CAP_SPIRAM)` (queues touched only from tasks,
   never ISR, are safe). Lowers the bring-up transient. Note: does **not** raise
   the steady floor (self-levels — see the trap above), but de-risks verify-boot.
   Safe. **Applied.**
6. **`MEMPOOL_PREFER_SPIRAM` — TRIED AND REVERTED.** Frees ~72 KB steady but the
   SDIO TX-from-PSRAM is unstable here (idle transport restart). Do not re-enable
   without an RX-only upstream option. See the "movable but don't" note above.
7. **Task stacks → PSRAM** (`xTaskCreateWithCaps`) — only for tasks safe during
   cache-disable; high risk, and only helps the transient (self-levels at steady),
   so low value given the transient is already safe. Not pursued.
8. **Move the BLE host to the C6** — the only large architectural win (frees the
   P4's NimBLE memory), but a major rework (C6 firmware + RPC BLE API),
   unverified in esp_hosted here.

**Host-stack note:** NimBLE *is* the lightweight host. IDF 6.0's `choice BT_HOST`
(`components/bt/Kconfig`) offers only NimBLE / Bluedroid / controller-only —
**there is no "ESP-BLE-HOST"**; Bluedroid is heavier. `esp_ble_mesh` /
`esp_ble_audio` are profile layers, not host stacks. Switching hosts does not
save internal DRAM.
