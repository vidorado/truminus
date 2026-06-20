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
  | **Handshake characteristic** | `9d667ea8-9c95-4dd0-b952-92031c4f5375` | WRITE. `discoverServices` calls `writeId()` here once: writes the advertised service-data bytes + a 4-byte device-id hash (`SharedData.getDeviceIdBytes`, a `_hash32` of `androidId_model…`). **Not** used for commands. |
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
| 4–5   | `BatteryType` | W | 0 |
| 6–7   | `Power` | W | constant **1** (power available/enabled) |
| 8–9   | `TempScale` | W | 0 (°C) |
| 10–11 | `PowerState` | W | **0 = OFF, 1 = ON** |
| 12–13 | `Mode` | W | **0 = AUTO, 2 = MAN** (1/ECO not yet observed) |
| 14–15 | `Temp` | W | **tenths of °C** — 210 = 21.0 °C, step 10 = 1.0 °C (gauge min 16.0) |
| 16–17 | `BlowerSpeed` | W | fan level, observed 1 and 3 |
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

Structure and the command direction are now CONFIRMED (see the byte table). Remaining unknowns:

1. Full `Mode` enum — only AUTO=0 and MAN=2 observed; 1 (likely ECO) and any others not yet seen.
2. `BlowerSpeed` full range (1 and 3 seen), `LedColor` palette (1, 3, 10 seen), `Flaps1/2Mode`
   beyond 0/1, and `TempScale` °C/°F codes.
3. Real-world telemetry **scaling**: what raw value the **physical** unit reports per field
   (`BatteryValue`, `Sonda*`, `Errors` bitmap, RPMs). The simulator cannot supply this — needs the unit.
4. ~~`Errors` → `MiError` mapping~~ — **DECODED** from the disassembly (see below).
   Only the raw "no fault" sentinel and the exact `Lb` (Low Battery) trigger still want the unit.

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
3. Handshake (matches the app's `writeId`): write the device-id (`SharedData.getDeviceIdBytes()`
   style: advertised service-data bytes + a 4-byte `_hash32` of an id string) to **`9d667ea8`**,
   then wait ~2 s. The app does this *before* it subscribes.
4. Subscribe to `4a01b4dd` (write CCCD `0x2902` = `0x0001`); decode each 124-char notification per
   the byte table and surface `Temp`/`Mode`/`Errors`/probes to the LCD + web UI.
5. To command: build the 30-byte write frame (`RealTimeClock` = current time-of-day seconds, then
   `PowerState`/`Mode`/`Temp`×10/`BlowerSpeed`/`LedBright`/`LedColor`/`ScheduledTime`/`Flaps1/2Mode`
   at the byte offsets above, with the reserved slot at 20–21 = 0), serialise big-endian as 2-digit
   hex, and `write()` the **ASCII-hex code units** to **`4a01b4dd`**.

NVS: follow the existing pattern (a `openair`/`ac` namespace with `addr`, panel hidden when
empty) — mirrors `solar`/`batt`/`multiplus` in the display skill.
