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
