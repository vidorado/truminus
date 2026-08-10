# TruMinus

Firmware for the **JC4880P443C** board (ESP32-P4) that emulates a Truma CP Plus D
control unit and manages a **Truma Combi D** heater over the LIN bus.

Control surfaces: a physical 800×480 LCD with capacitive touch, a WebSocket web
UI and a serial CLI.  Solar charge data (Victron BLE) and battery SOC
(Ultimatron BLE) are surfaced on both the LCD and the web UI, along with the
**actual boiler water temperature** — a reading the stock Truma CP Plus does
not expose.

For remote access through home/RV CGNAT, the firmware can dial out to a
companion Node.js bridge — [`vidorado/truminus-cloud-server`](https://github.com/vidorado/truminus-cloud-server),
which you run on any VPS or a Plesk-managed subdomain like
`tunnel.yourdomain.com` — and tunnel browser traffic over WSS — no port
forwarding, no DDNS, just a domain you control.

> This software is not provided, endorsed, supported, or sponsored by Truma.
> See [LICENSE](LICENSE) — no warranty of any kind.

---

## Hardware
![JC4880P443C board with LIN interface / PS](doc/screen-with-lin-interface.jpg)
| | |
|---|---|
| **Board** | JC4880P443C (ESP32-P4-WROOM, dual-core RISC-V @ 400 MHz) |
| **Flash / PSRAM** | 16 MB QIO / 32 MB HEX |
| **Display** | 4.3" ST7701 RGB panel, 800×480 landscape |
| **Touch** | GT911 capacitive, I2C |
| **Connectivity** | WiFi + BLE 5 via co-processor ESP32-C6 (SDIO) |
| **Upload** | USB-CDC on `/dev/ttyACM0` (no USB bridge needed) |

The AM2301/DHT22 external temperature sensor pin is still being finalised on the
new board — check `main/main.cpp` for current assignments.

### Hardware: P4 ↔ Truma LIN wiring

TruMinus emulates the **CP-Plus D (the LIN bus master)** and talks to the Truma
Combi D over a single-wire LIN bus through a **TJA1020 LIN transceiver** module.

**ESP32-P4 ↔ transceiver (UART1 @ 9600 baud, 8N1):**

| ESP32-P4 | Transceiver | Notes |
|----------|-------------|-------|
| GPIO27 (TX) | TXD | + **pull-up required** (see below) |
| GPIO26 (RX) | RXD | open-drain output → + **pull-up required** |
| GND | GND | must be common with battery − and Truma GND |

(Pins are defined as `LIN_TX_PIN` / `LIN_RX_PIN` in `main/main.cpp`.)

**Transceiver ↔ Truma / power:**

- The TJA1020 is powered from **+12 V (battery positive)**, *not* from the Truma
  connector. Only the **LIN** line and **GND** run to the Truma's CP-Plus port.
- **All grounds must be common**: battery −, the P4 board GND, and the Truma GND.

> [!IMPORTANT]
> **Both logic lines need external pull-ups — this is not optional.**
> - **RXD** is an **open-drain** output: without a pull-up it never goes HIGH.
> - **TXD** has only a weak **internal pull-down** (125–800 kΩ, no pull-up), so
>   the host alone must drive the recessive (HIGH) level. On the P4 the recessive
>   edge rises too slowly without help: the recessive bits of our own transmission
>   sag below the TXD threshold (`VIH = 2 V`) and **every received frame fails its
>   checksum** (you read `0x00` / garbage even though the heater reacts to commands).
>
> Fit a pull-up on **each** line. **4.7 kΩ to 5 V works** (the TJA1020 TX/RX pins
> are 5 V-tolerant — abs-max 7 V). **2.2–4.7 kΩ to 3.3 V is cleaner**: the
> transceiver only needs > 2 V, and 3.3 V avoids over-driving the P4's
> non-5 V-tolerant GPIOs (with a 5 V pull-up the pin's clamp diode holds it at
> ~3.6 V — works, but slightly out of spec).
>
> Symptom if a pull-up is missing/weak: scope **TXD** — a *rounded* rising edge
> (instead of a sharp one) means the recessive level is being starved. Build with
> `-DLIN_SELFTEST` to dump a baud-sweep loopback test at boot for diagnosis.

---

## Prerequisites

**ESP-IDF 6.0.1** (the `release/v6.0` branch). PlatformIO is not used — the build
is driven directly by `idf.py`.

```bash
# Clone IDF (once per machine)
git clone --branch release/v6.0 \
    https://github.com/espressif/esp-idf.git ~/esp/esp-idf

# Install the ESP32-P4 toolchain (once per machine)
cd ~/esp/esp-idf
./install.sh esp32p4
```

---

## Building

Source the IDF environment **once per terminal session**, then use `make` or
`idf.py` directly:

```bash
. ~/esp/esp-idf/export.sh

make               # build
make flash         # build + flash  (PORT defaults to /dev/ttyACM0)
make monitor       # serial monitor at 115200
make flash-monitor # flash then open monitor
make clean         # idf.py fullclean

PORT=/dev/ttyUSB0 make flash   # override port if needed
```

Equivalent `idf.py` commands if you prefer them directly:

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
idf.py fullclean
```

Web assets under `data/` are **not** part of the firmware binary: they are built
into a LittleFS image and flashed to a dedicated partition. `idf.py flash`
includes it, but after a change to `data/` alone you can reflash just the assets:

```bash
idf.py littlefs-flash-littlefs
```

The image is regenerated on every build, with cache-busting hashes rewritten and
compressible files pre-gzipped alongside the originals.

---

## VSCode

Install the **ESP-IDF extension** (`espressif.esp-idf-extension`) — it provides
build, flash and monitor buttons without any additional wrapper tooling. The
`.vscode/extensions.json` in this repo will suggest it automatically when you
open the folder.

After installing the extension, create your local settings file from the
template:

```bash
cp .vscode/settings.json.template .vscode/settings.json
# then edit settings.json and replace YOUR_USER with your username
```

`settings.json` is gitignored so each developer keeps their own paths locally.

After the first successful `make build` (or `idf.py build`), the file
`build/compile_commands.json` is generated and IntelliSense will pick it up
automatically.

---

## Firmware updates (OTA)

After the first cable flash the device updates itself over WiFi — no cable,
no port forwarding.  It checks this repo's **GitHub Releases** at boot and
every 12 h and compares its built-in version against the latest tag.

- **You're only nagged for meaningful updates.**  A pop-up + a topbar icon
  appear automatically only for **minor/major** version bumps.  **Patch**
  releases are silent — you still find and install them on demand from
  **[⚙ Config] → Actualizaciones** (or the CLI below).
- **Installs are user-initiated** (an *Update* button on the LCD / web), with
  a progress bar.  The transfer pauses BLE and the tunnel so the download
  isn't starved.
- **Safe by design:** a freshly flashed image must pass a self-test (heap +
  task liveness) within ~1 min or the bootloader **rolls back** to the
  previous image.  A reverted update logs its reason on the next boot.

> **First flash must be by cable** (`make flash`).  An update only ships the
> P4 application image; the very first install of a brand-new feature that
> changes the *downloader itself* still needs one cable flash to take effect.

### Publishing a release (maintainers)

```bash
git tag X.Y.Z && git push origin X.Y.Z
```

GitHub Actions (`.github/workflows/release.yml`) builds the firmware and
attaches the binary as an asset named **exactly `truminus.bin`** — the
firmware downloads it by that name, so the name is a contract.  Tag from a
**clean** tree so the embedded version is exactly `X.Y.Z` (a dirty tree bakes
`X.Y.Z-<n>-g<sha>-dirty`).  Builds are ccache-accelerated via a parallel
`master` CI build (~2.5 min per release).

---

## Serial CLI

A line-based REPL is available on the USB-Serial-JTAG console
(`/dev/ttyACM0`, e.g. `make monitor`).  Commands:

| Command | Purpose |
|---|---|
| `wifi <ssid> [pass]` | Connect + persist WiFi credentials |
| `tunnel <on\|off>` | Enable/disable the WSS reverse tunnel |
| `victron <mac> <key32hex>` | Set Victron solar BLE creds |
| `ultimatron <mac> [pass]` | Set Ultimatron BMS BLE creds |
| `tank <mac>\|clear` | Bind/unbind the BTHome tank sensor |
| `multiplus <mac> <key32hex>\|clear` | Set/clear the VE.Bus dongle |
| `ota [check\|install]` | Check for / install a firmware update |
| `show` | Print stored config |

> USB-Serial-JTAG runs in linenoise *dumb mode* (no arrow keys / history);
> BACKSPACE and Ctrl+U work.

---

## Architecture

```
Truma Combi D ←→ LIN transceiver ←→ ESP32-P4 UART
                    ↕
MQTT broker / WebSocket clients / Serial CLI / Touch UI
                    ↕
Victron BLE (solar) / Ultimatron BLE (battery)
```

Key source files in `main/`:

| File | Purpose |
|------|---------|
| `main.cpp` | `app_main` entry point (currently a display-only stub) |
| `p4display.cpp/.hpp` | LVGL UI for the 800×480 LCD |
| `lin_driver.cpp/.hpp` | Half-duplex LIN driver over ESP-IDF UART |
| `trumaframes.cpp/.hpp` | LIN protocol layer — frame parsing / building |
| `settings.cpp/.hpp` | Setpoint abstraction (heating / fan / boiler / energy) |
| `webserver.cpp/.hpp` | HTTP static serving + WebSocket JSON handler |
| `victronble.cpp/.hpp` | Victron SmartSolar BLE (Instant Readout protocol) |
| `ultimatronble.cpp/.hpp` | Ultimatron LiFePO4 BMS BLE (GATT) |
| `tankble.cpp/.hpp` | Fresh-water tank level — BLE BTHome v2 receiver |
| `multiplusble.cpp/.hpp` | Victron Multiplus / VE.Bus inverter — BLE Instant Readout receiver |
| `i18n.cpp/.hpp` | `TK` enum + `t(TK::KEY)` — Spanish/English, persisted in NVS |

Build system notes and known gotchas are in
[`.claude/skills/pio-idf-p4/SKILL.md`](.claude/skills/pio-idf-p4/SKILL.md).

---

## Public access via tunnel (companion repo)

The ESP sits behind home/RV CGNAT in most installs, so it isn't reachable
from the outside.  A small Node.js bridge — which you run on any public-IP
server (a VPS, a Plesk-managed subdomain, etc.) — accepts the firmware's
outbound WSS connection and muxes browser HTTP/WS traffic back to the
device's local `esp_http_server`.

> **The bridge lives in its own repository:**
> [`vidorado/truminus-cloud-server`](https://github.com/vidorado/truminus-cloud-server).
> Setup (Unix bare or Plesk Node.js), env vars and operational quirks
> (reverse cache, queue, Passenger keepalive) are documented there.  The
> wire protocol lives in
> [`.claude/skills/wss-tunnel/SKILL.md`](.claude/skills/wss-tunnel/SKILL.md)
> (firmware side) and in the companion repo's
> `.claude/skills/tunnel-bridge/SKILL.md` (server side).

### Configure the device

On the TruMinus LCD: **[⚙ Config]** → **Tunnel**:
- **Domain**: bare hostname, e.g. `tunnel.example.com` (no scheme, no
  path — the firmware composes `wss://<domain>/tunnel?token=…` itself).
- **Token**: the same value as `TUNNEL_TOKEN` on the server.
- **Enable switch → ON**
- Save.

The cloud icon in the top bar blinks while the WSS handshake is in
flight, turns solid blue when up, and goes red after
3 consecutive disconnects without a successful connect.

---

## Fresh-water tank sensor (BLE / BTHome v2)

The right-most panel on the screen (and the matching widget in the web
UI) shows the fill level of the fresh-water tank as a percentage.  The
value comes from any BLE device that broadcasts a [BTHome v2](https://bthome.io)
unencrypted Service Data frame (UUID `0xFCD2`) carrying tag `0x2F`
(*Moisture*, `uint8` 0..100 %).  Same wire format Home Assistant and
ESPHome use for their generic moisture sensors, so anything that can
emit BTHome works as a sensor — including the
[TruMinus-HWSim](https://github.com/seguridad2000/TruMinus-HWSim)
ESP32-C3 simulator we use on the bench.

Pairing is one-shot: the firmware filters incoming ads by source MAC,
so you tell it which MAC to trust and it ignores everything else.

### Pair a sensor

1. Power up the sensor near the TruMinus device.
2. On the LCD: **[⚙ Config]** → **Monitorización** → scroll to *Sensor
   depósito agua*.  Tap the 🔍 button next to the MAC field and pick
   your device from the scan list, then **Save**.
3. Or from the serial REPL (USB-Serial-JTAG on `/dev/ttyACM0`):
   ```
   tank AABBCCDDEEFF      # paste the MAC of your sensor
   show                   # confirm: "tank: addr=AABBCCDDEEFF"
   ```
   Use `tank clear` to unpair.

The widget shows `-- %` until the first valid frame arrives (typically
within one BLE scan window, ~5 s).  Fill colour follows the same
ramp as the battery: red below 20 %, amber 20–49 %, blue 50 % and
above.

---

## Multiplus / VE.Bus inverter (BLE Instant Readout)

The right-most panel on row 2 mirrors the Victron Multiplus dashboard:
shore (`RED`), inverter status, AC load (`CARGA`) and DC battery side
(`BAT.`).  Data comes from the *VE.Bus Smart* dongle the Multiplus has
plugged into its accessory port — that dongle is what advertises over
BLE; the Multiplus itself does not.

Wire side is the same Instant Readout protocol that the Solar panel
already uses, but with **record type `0x0C`** (VE.Bus) instead of `0x01`
(Solar), a **per-device AES-128 bind key** retrieved from VictronConnect
→ device → Product info, and a **packed 102-bit plaintext** that
includes the inverter operation mode, shore-side power, load-side
power, battery V/A/T, AC-in state, alarm flags and SOC.  Full byte map
in [`.claude/skills/multiplusble/SKILL.md`](.claude/skills/multiplusble/SKILL.md).

Read-only: the **On / Off** buttons render disabled because turning
the Multiplus on or off requires VE.Bus over BLE GATT, a protocol
Victron has not documented and which no open-source library
(`keshavdv/victron-ble`, `Fabian-Schmidt/esphome-victron_ble`, …) has
reverse-engineered.

---

## Relation to upstream

This project started as a fork of **[olivluca/TruMinus](https://github.com/olivluca/TruMinus)**
(CP Plus emulator + MQTT/web UI) and **[olivluca/TrumaDisplay](https://github.com/olivluca/TrumaDisplay)**
(CYD touch UI), merging both into a single firmware. The LIN protocol work, MQTT
topic layout, Home Assistant autodiscovery and the original web interface all come
from olivluca's work.

The previous board (ESP32-C5 / NM-CYD-C5) is preserved in the git history: see
the commits before `4d30c6b` ("migrate from CYD-C5 to JC4880-P4").

---

## Credits

- **[olivluca/TruMinus](https://github.com/olivluca/TruMinus)** — original CP Plus emulator, LIN protocol and web UI
- **[olivluca/TrumaDisplay](https://github.com/olivluca/TrumaDisplay)** — CYD touch UI reference
- **[danielfett/inetbox.py](https://github.com/danielfett/inetbox.py)** — LIN transceiver wiring reference
- **[chrisj7903/Read-Victron-advertised-data](https://github.com/chrisj7903/Read-Victron-advertised-data)** — Victron Instant Readout reference (Solar / BMV)
- **[keshavdv/victron-ble](https://github.com/keshavdv/victron-ble)** — Python parser used as reference for the VE.Bus record format
- **[Fabian-Schmidt/esphome-victron_ble](https://github.com/Fabian-Schmidt/esphome-victron_ble)** — confirmed the C struct layout of the Instant Readout envelope
- **[sergkh/node-ultimatron-battery](https://github.com/sergkh/node-ultimatron-battery)** — Ultimatron BMS GATT protocol reference

## License

GPL — see [LICENSE](LICENSE).
