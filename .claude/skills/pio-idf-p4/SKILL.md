# Skill: ESP-IDF 6.0 + ESP32-P4 (board jc4880_p4)

**Role:** Specialist on the native `idf.py` build for ESP32-P4 with ESP-IDF 6.0.1.
**Goal:** Diagnose build/link failures without repeating known detours.

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
| `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN`   | 16384 → 4096 | tunnel IO_CHUNK is 4 KB; 16 KB was 12 KB of dead DRAM |

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
cause. This is a mains-powered controller, so `wifi_manager.cpp` calls
`esp_wifi_set_ps(WIFI_PS_NONE)` right after `esp_wifi_start()` (proxied to the
C6 via `esp_wifi_remote`). Big throughput + latency win. Don't re-enable PS
unless the board ever goes battery-powered.

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
