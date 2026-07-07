# OpenAir PLUS — Bergstrom/Dirna A/C BLE protocol

How to talk to a Bergstrom / Dirna **OpenAir PLUS** camper air-conditioning unit over BLE,
reverse-engineered from the official Android app `com.bergstrom.openair`.

> **Source of truth:** the app is Flutter; the protocol lives in the AOT Dart blob
> `lib/arm64-v8a/libapp.so` (Dart **3.9.2**, Flutter stable, Aug 2025). It was decompiled
> with **Blutter** → `../OpenAirPLUS/out_blutter/asm/flutter_proyecto/`. The Dart sources of
> record are `classes/bluetooth_service.dart`, `classes/Mensaje.dart`,
> `classes/variables.dart`, `conversion_datos.dart`. Everything marked **CONFIRMED** was read
> straight from that disassembly; items marked **TO VERIFY** still want a `btsnoop_hci.log`
> capture against a real unit to pin down exact value encodings.

---

## How to reproduce the analysis (if a newer APK appears)

```bash
# 1. Pull the ABI split that actually contains libapp.so (base.apk does NOT)
adb shell pm path com.bergstrom.openair          # lists base + split_config.*.apk
adb pull .../split_config.arm64_v8a.apk
python3 -c "import zipfile; zipfile.ZipFile('split_config.arm64_v8a.apk').extractall('x',[n for n in zipfile.ZipFile('split_config.arm64_v8a.apk').namelist() if n.startswith('lib/')])"
# 2. Decompile (needs libcapstone-dev libicu-dev pkg-config g++>=13; clones+builds Dart SDK)
git clone --depth 1 https://github.com/worawit/blutter
python3 blutter/blutter.py x/lib/arm64-v8a out_blutter   # → out_blutter/asm/...
```
WSL note: the phone reaches WSL via `usbipd attach --wsl --busid <n>`; enable **USB debugging**
on the phone and accept the RSA prompt or `adb` shows `unauthorized`. A WSL restart drops both
the usbipd attach and any background job — Blutter's build is incremental, just rerun it.

---

## Transport — CONFIRMED

- **BLE GATT central**, plugin **flutter_blue_plus**. App scans, connects, discovers services,
  subscribes to notify, writes commands. **Advertised (GAP) device name: `My OpenAir PLUS`** —
  the scan screen (`bt_screen.dart`) does an unfiltered `startScan` then filters results in Dart
  by `advName` against the exact literal `"My OpenAir PLUS"`, so a peripheral must advertise that
  name (not `OpenAir PLUS`) to appear in the app's list. (`OpenAir PLUS` is only the app label.)
- **GATT layout** (roles nailed down in `bluetooth_service.dart`):

  | Role | UUID | How identified |
  |------|------|----------------|
  | **Service** | `e43ff2c2-8602-48f6-82d0-72cd56fb06f2` | `services.firstWhere((s) => s.uuid == …)` |
  | **Handshake characteristic** | `9d667ea8-9c95-4dd0-b952-92031c4f5375` | WRITE. `discoverServices` calls `writeId()` here once, *before* subscribing: writes an **8-byte device id** (real-unit capture, see Handshake below). **Not** used for commands. |
  | **Data characteristic** (telemetry **and** commands) | `4a01b4dd-350d-4afc-9a9f-27164f2b6b56` | NOTIFY + READ + **WRITE**, bidirectional. The app `setNotifyValue(true)` + `onValueReceived()` for telemetry, **and** caches this UUID as the write target (`field_4b`, debug `"CARAC DE ESC?"`); `writeMessage()` writes every command here. |
  | CCCD | `00002902-0000-1000-8000-00805f9b34fb` | standard, enables notifications |

- App permissions: `BLUETOOTH(_ADMIN/_SCAN/_CONNECT)`, `ACCESS_FINE/COARSE_LOCATION`,
  feature `bluetooth_le`. Connection is plain GATT — **no app-layer pairing/bonding PIN** seen.

---

## The `Mensaje` frame — CONFIRMED structure

State and commands are one record, class `Mensaje` (`classes/Mensaje.dart`). It is an ordered
array of **28 fields**. Canonical order (index = position in the frame), taken verbatim from
`Mensaje` (the `devNombreVariable(pos)` / field-name table):

| # | Field | Dir | Meaning |
|---|-------|-----|---------|
| 0 | `RealTimeClock` | W | time-of-day, computed `h*3600 + m*60 + s` (seconds since midnight) |
| 1 | `BatteryType` | W | battery chemistry/type selector |
| 2 | `Power` | W | power command |
| 3 | `TempScale` | W | °C / °F selector |
| 4 | `PowerState` | W | on/off state |
| 5 | `Mode` | W | **AUTO / ECO / MAN** (manual) — UI labels from `acc_mode_toggle_button.dart` |
| 6 | `Temp` | W | **target temperature setpoint** |
| 7 | `BlowerSpeed` | W | **fan / blower speed** |
| 8 | `LedBright` | W | panel LED brightness |
| 9 | `LedColor` | W | panel LED colour |
| 10 | `ScheduledTime` | W | timer / scheduled on time |
| 11 | `Flaps1Mode` | W | **louver/swing 1 mode** |
| 12 | `Flaps2Mode` | W | **louver/swing 2 mode** |
| 13 | `Errors` | R | error/fault bitfield (UI: warnings screen) |
| 14 | `BatteryValue` | R | battery voltage/SOC |
| 15 | `ElectroSpeed` | R | electro (DC compressor) speed |
| 16 | `CompressorSpeed` | R | compressor speed |
| 17 | `BlowerSpeedPer` | R | blower speed % |
| 18 | `ElectroSpeedPer` | R | electro speed % |
| 19 | `CompressorSpeedRPM` | R | compressor RPM |
| 20 | `TiltX` | R | accelerometer tilt X |
| 21 | `TiltY` | R | accelerometer tilt Y |
| 22 | `Sonda1F` | R | probe 1 temperature °F |
| 23 | `Sonda1C` | R | probe 1 temperature °C |
| 24 | `Sonda2F` | R | probe 2 temperature °F |
| 25 | `Sonda2C` | R | probe 2 temperature °C |
| 26 | `VersionBase` | R | base board firmware version |
| 27 | `VersionFrontal` | R | front panel firmware version |

**The write frame is only the first 13 fields (0–12).** `Mensaje.devMensajeEscribir()` loops
exactly 13 times (`cmp #0xd`) building the outgoing `List<int>`, starting with `RealTimeClock`
derived from the phone clock. Fields 13–27 are **read-only telemetry** parsed from notifications.

### Byte-level layout — CONFIRMED by live capture (simulator + app)

Captured by impersonating the unit (see "Simulator" below), feeding an incremental probe and
reading the app's field dump in logcat, then tapping every control and decoding the writes.

**Wire format:** ASCII-hex text, two hex chars per byte, **big-endian** per field. The notify
parser is `deAsciiAInt` (splits the char string into 2-char→1-byte values) → `cargarDesdeDispositivo`.

**Read frame (notifications, device→app): 62 bytes = 124 ASCII chars.** `deAsciiAInt` produces
`(len-4)/2` values and `cargarDesdeDispositivo` requires **exactly 60** (`Mensaje.field_23.length`);
the frame must therefore be **124 chars** (60 data bytes decoded + the last 4 chars discarded). A
wrong length only prints `"…no coinciden en longitud."` (non-fatal) but leaves `Errors` as an `int`,
which then crashes the notify stream with `type 'int' is not a subtype of type 'MiError'`.

**Write frame (commands, app→device): 30 bytes = 60 ASCII chars** — the first 30 bytes of the same
layout (through `Flaps2Mode`).

| Bytes | Field | W/R | Confirmed values / scaling |
|-------|-------|-----|----------------------------|
| 0–3   | `RealTimeClock` (u32) | W | seconds since midnight, from the **phone** clock (e.g. 56075 = 15:34:35) |
| 4–5   | `BatteryType` | W | constant **1** (in the real-unit capture; app never exposes it) |
| 6–7   | `Power` | W | constant **0** (real-unit capture — command frames send 0, not 1) |
| 8–9   | `TempScale` | W | 0 (°C) |
| 10–11 | `PowerState` | W | **0 = OFF, 1 = ON** |
| 12–13 | `Mode` | W | **0 = AUTO, 2 = MAN** (1/ECO not yet observed) |
| 14–15 | `Temp` | W | **tenths of °C** — 210 = 21.0 °C, step 10 = 1.0 °C (gauge min 16.0) |
| 16–17 | `BlowerSpeed` | W | fan level, observed 1–4 |
| 18–19 | `LedBright` | W | 0/1 |
| 20–21 | *(reserved)* | — | always 0; **not** surfaced by the app, but occupies a slot in both frames |
| 22–23 | `LedColor` | W | colour index, observed 1, 3, 10 |
| 24–25 | `ScheduledTime` (u16) | W | timer value, e.g. 3577 |
| 26–27 | `Flaps1Mode` | W | 0/1 |
| 28–29 | `Flaps2Mode` | W | 0/1 |
| 30–61 | telemetry (`Errors`, `BatteryValue`, …`VersionFrontal`) | R | read-only; only in the 62-byte read frame |

> ⚠️ The 28-field table above is the logical `Mensaje` model; the **wire layout is not 1 byte/field**.
> `RealTimeClock` is 4 bytes and there is a reserved 2-byte slot at offset 20, so the named fields
> sit at the byte offsets in this table, not at `index*2`.

- `conversion_datos.dart` formats values back with `toRadixString(16)` left-padded — `devMensaje2Bytes`
  / `devMensaje4Bytes` emit the 2- and 4-byte fields big-endian.
- The length-mismatch guard (`"…no coinciden en longitud."`) requires the read frame to be exactly
  124 chars (see byte layout above).
- **CONFIRMED:** the command write is `writeChar(4a01b4dd).write(value)` (`writeMessage`), and
  `value` is the **ASCII-hex string's code units** (the captured bytes are `30 30 30 30 …` = `"0000…"`),
  30 bytes of model → 60 ASCII chars. `RealTimeClock` is 4 bytes, `ScheduledTime` 2 bytes, big-endian.

---

## Still TO VERIFY (values, not structure)

Structure and the command direction are CONFIRMED (see the byte table). After a second
disassembly pass, most "unknowns" were resolved from the app — only one truly needs the unit:

1. ~~Full `Mode` enum~~ — **CONFIRMED `AUTO=0, ECO=1, MAN=2`**. `acc_mode_toggle_button.dart`
   builds the label list in order `["AUTO","ECO","MAN"]` indexed by the Mode value.
2. `BlowerSpeed`/`LedColor`/`Flaps`/`TempScale` full ranges — only matter for *displaying* every
   option the unit offers; irrelevant for our firmware, which only ever **sends** valid values
   chosen in our own UI (blower 1–6, etc.). Not blocking.
3. Real-world telemetry **scaling** — mostly resolved by the real-unit capture, one probe unit
   still open:
   - **`BatteryValue` = millivolts (÷1000 = volts).** CONFIRMED: the capture reads 13373–13534,
     i.e. **13.4–13.5 V** on a 12 V system.
   - `CompressorSpeedRPM` reads plausible raw RPM (0 / 2100 / 2800), no scaling.
   - `BlowerSpeedPer`/`ElectroSpeedPer` are already **percent** (0–54 observed).
   - **Probe temperatures still open.** The app never displays `Sonda1C`/`Sonda2C` (parsed in
     `Mensaje.dart`, never shown), so there is no raw→°C conversion to copy, and the capture's
     `Sonda*` values (`Sonda1C` 752–764, `Sonda2C` 376–650) don't map cleanly to °C at ÷10 nor do
     the paired `F`/`C` fields agree as a unit conversion. Needs a capture correlated with a known
     probe reading. The firmware exposes the raw `int16` for now.
4. ~~`Errors` → `MiError` mapping~~ — **DECODED** (see catalog below). The raw "no fault" sentinel
   stays the only ambiguity (firmware treats `0` as no-fault, the safe default).

### Connect + handshake — CONFIRMED end-to-end against the real unit

A `btsnoop_hci.log` of the app pairing with a real OpenAir unit (Pixel 8, Android 16) plus a
**working TruMinus firmware connection** settle the whole flow. The exact winning ATT sequence:

```
MTU exchange           (app requests 517, unit grants 256)
WriteReq  h=0x0009 = 0002   enable INDICATIONS on Service Changed (0x2A05, GATT svc 0x1801)
WriteReq  h=0x0013 = <8-byte id>     the handshake (writeId)
  … unit does NOT answer until the user long-presses the unit's ON/OFF button …
WriteRsp                    arrives only after the human confirms (seconds later)
WriteReq  h=0x0011 = 0001   enable NOTIFICATIONS on the data char (0x4a01b4dd)
Notify    h=0x0010 = <124-char telemetry>   flows from here on, unsolicited
```

Three things the firmware MUST do, each confirmed necessary by the capture:

1. **Enable Service Changed indications (`0x2A05` CCCD = `0x0002`) BEFORE the handshake.** The app
   always does this first; skipping it was one reason our early attempts were rejected.
2. **Handshake = exactly 8 raw id bytes, Write *Request* (response=true).** No service-data prefix.
   The char is Write-only (`props 0x08`). A write-*without*-response is dropped. The captured id
   `01 e8 77 07 ed e0 c8 25` is **identical across two independent captures** (different units), so
   it is effectively a **constant**, not a per-phone hash — the firmware hardcodes it as
   `TRUMINUS_ID8`. (The decompiled `getDeviceIdBytes` = `_hash32("androidId_brand_model")` LE + 4
   zeros; the constant observed value means `androidId` resolves to a fixed default here.)
3. **Wait for the WriteRsp with a long timeout (≥30 s).** The unit does **not** answer the handshake
   until the user **long-presses the ON/OFF button** on the console (it shows `Bt`) — this is the
   human pairing confirmation. NimBLE's default ATT timeout (~30 s) covers it. Enable data-char
   notifications immediately after the WriteRsp.

**Pairing UX:** first pairing needs the long-press (`Bt`) while the firmware is mid-handshake — the
board keeps the connection open waiting for the WriteRsp. Once the unit has stored our
`TRUMINUS_ID8`, later reconnects are accepted without the long-press (known client). A **hard power
cycle of the unit** helps it re-enter the pairing/`Bt` state cleanly. Surface a **pairing-assistant
screen** after device selection: try to connect, and if it doesn't succeed within a few seconds show
"long-press the unit's ON/OFF button", keep retrying, and allow cancel.

> ⚠️ **Use a short-lived cyclic poll, NOT a persistent connection.** `openairPollOnce()` connects,
> reads one telemetry frame, sends any pending command, reads back, and disconnects — every
> `POLL_INTERVAL_MS` (15 s) in steady state, or immediately when a command is pending. A persistent
> GATT link was tried and abandoned: holding a connection open while the supervisor runs its
> continuous Victron scan destabilised the **shared ESP32-C6 radio/SDIO link** and reliably crashed
> the P4 (`assert failed: sdio_rx_get_buffer sdio_drv.c:953` at ~6.6 s, boot-loop). The base firmware
> is stable; the persistent-link changes introduced the crash, so the cyclic model is the safe design.
> (The idea that the unit powers off when the link drops was an unverified hunch — the cyclic poll
> works; the only visible cost is the unit briefly showing "Bt" per poll, kept infrequent by the gate.)

> ⚠️ **Do NOT write a setpoint frame every poll.** The unit re-applies any command it receives
> (audible **beep** + LED flash). Telemetry notifications arrive on their own once subscribed, so
> only write the 60-char command frame when the user actually changed something (`s_hasPendingCmd`).

> ⚠️ **Build commands ON TOP of the unit's own last state — never from defaults.** A command frame
> carries ALL 13 writable fields, so any field you don't copy from the unit reconfigures it. Hardcoding
> `BatteryType=0`/`Power=1` was wrong (the real unit runs `BatteryType=1` lithium, `Power=0`) and
> visibly disturbed operation (fan stutter, config drift). The firmware snapshots the first 30 bytes of
> each telemetry frame (`s_writeShadow`) and overwrites ONLY the four user controls
> (PowerState/Mode/Temp/BlowerSpeed) + the clock; everything else (BatteryType, Power, TempScale,
> LED, ScheduledTime, Flaps) is echoed back byte-for-byte. Never send a command before the first
> telemetry read (`s_haveShadow`).

### Config fields (BatteryType, Power) — change only while the unit is OFF

`Power` = max A/C power (`0` = 1.2 kW, `1` = 2.0 kW). `BatteryType` = battery chemistry (`1` = lithium).
The **official app forbids changing either while the unit is running** — doing it hot is harmful. So in
TruMinus these are **configuration settings on the Peripherals screen**, not live panel buttons, and are
only written when the unit is OFF; while running we always mirror whatever telemetry reports (the
`s_writeShadow` echo already guarantees this). Applies to both the web UI and the LCD (drop the old
`1.2`/`2.0` power buttons from the CLIMATIZACIÓN panel).

### `Errors` → `MiError` catalog — CONFIRMED (decoded from `error.dart::listaErrores`)

`Mensaje.dart` formats the raw `Errors` value with `toRadixString(16).padLeft(2,'0')`
(`_toPow2String(16)` + `padLeft`) and does a `singleWhere` over `listaErrores` by that
**2-digit hex** code; an unmatched code falls back to `"Error no encontrado"`. So the catalog
codes are **hex** — `"12"`/`"13"` are decimal **18/19**, not 12/13.

| Code (hex) | Raw int | Title | Severity (`field_13`) | App behaviour text |
|-----------|---------|-------|-----------------------|--------------------|
| `00` | 0  | Recirculation probe Error | 0 (warning) | Works without external temp control, limited operation. Visit workshop. |
| `01` | 1  | Electric-Fan Error        | 2 (critical) | **Cannot work** until solved. Workshop. |
| `02` | 2  | Blower Error              | 2 (critical) | **Cannot work** until solved. Workshop. |
| `06` | 6  | Freeze Probe Error       | 2 (critical) | **Cannot work** until solved. Workshop. |
| `09` | 9  | Tilt Error               | 1 (temporary) | Vehicle over-tilted; auto-recovers when level. |
| `12` | 18 | Flap 1 Error             | 0 (warning) | Works, limited operation. Workshop. |
| `13` | 19 | Flap 2 Error             | 0 (warning) | Works, limited operation. Workshop. |
| `Lb` | —  | Low Battery              | 1 (temporary) | Battery insufficient; auto-recovers when charged. |

Severity (`MiError.field_13`): **0** = warning / limited operation · **1** = temporary,
auto-recovering · **2** = critical, unit stops. `Lb` is a **special non-numeric flag** (not a
hex code) — how it is derived from the frame is still unverified.

> ⚠️ Raw `Errors == 0` is ambiguous: the catalog maps `00` → "Recirculation probe Error", but the
> app has a *separate* "NO ERROR" branch, so a healthy unit likely reports a different sentinel
> (not 0). The firmware therefore treats `errors == 0` as **no fault** (shows nothing) to avoid a
> permanent false alarm; confirm the real no-fault value against the unit before trusting `00`.

### Primary method — the OpenAir simulator (a priori, no real unit) — DONE for commands

A capture personality lives in the **`truminus-hw-simulator`** repo (ESP32-C3, NimBLE):
`make openair-flash-monitor`. It advertises as **`My OpenAir PLUS`** with the exact service/chars
above (`4a01b4dd` is registered WRITE+NOTIFY+READ so the app's command writes land on it), pushes
a valid 124-char telemetry frame so the home screen renders, and logs+decodes every command write.
This recovered the entire **command** protocol above without the hardware or a sniffer:

- Tap *Power on/off*, *Mode AUTO/MAN*, *Temp ±*, fan, LED, each flap — the monitor prints the
  decoded frame (`PowerState=`, `Mode=`, `Temp=… (21.0 C)`, …). Diff across taps to read off enums
  and scaling. Power-state/mode/temp/fan/LED/flaps/scheduled-time are all confirmed this way.
- Push telemetry with `oaset <idx> <val>` (e.g. `oaset 6 210` = 21.0 °C) and watch the app render.

### Fallback — `btsnoop_hci.log` against the real unit (when available)

Phone Developer Options → "Enable Bluetooth HCI snoop log", drive each control once next to the
unit, pull `btsnoop_hci.log`, open in Wireshark (`btatt`), diff writes/reads on `4a01b4dd`. This is
the only way to get the real telemetry **scaling** (item 3).

---

## Porting into TruMinus firmware

This is a NimBLE **central** role (the firmware already runs NimBLE 2.x for Victron/Ultimatron,
but only as an observer/scanner — this needs an active GATT client connection):

1. Scan, match advertised name `My OpenAir PLUS`, connect.
2. Discover service `e43ff2c2-…`; cache handshake char `9d667ea8-…` and data char `4a01b4dd-…`.
3. Handshake (matches the app's `writeId`): write a **stable 8-byte id** to **`9d667ea8`** (raw
   bytes, no service-data prefix, no trailing zeros — see Handshake above), then wait ~2 s. The
   app does this *before* it subscribes. **First-connect pairing:** the unit only stores a new id
   in pairing mode, entered by a **long-press on its power button** — instruct the user to do this
   when they first select the device in our settings UI, then watch the board monitor to confirm
   the handshake + first telemetry frame land.
4. Subscribe to `4a01b4dd` (write CCCD `0x2902` = `0x0001`); decode each 124-char notification per
   the byte table and surface `Temp`/`Mode`/`Errors`/probes to the LCD + web UI.
5. To command: build the 30-byte write frame (`RealTimeClock` = current time-of-day seconds, then
   `PowerState`/`Mode`/`Temp`×10/`BlowerSpeed`/`LedBright`/`LedColor`/`ScheduledTime`/`Flaps1/2Mode`
   at the byte offsets above, with the reserved slot at 20–21 = 0), serialise big-endian as 2-digit
   hex, and `write()` the **ASCII-hex code units** to **`4a01b4dd`**.

NVS: follow the existing pattern (a `openair`/`ac` namespace with `addr`, panel hidden when
empty) — mirrors `solar`/`batt`/`multiplus` in the display skill.
