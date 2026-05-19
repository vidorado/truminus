# Truma Combi D — Full LIN bus protocol

Technical reference for the communication protocol between TruMinus (emulated CP Plus D) and the Truma Combi D.
Everything here was confirmed either by passive LIN bus captures or by direct experimentation on the real device.

---

## High-level architecture

```
Truma Combi D ←→ LIN transceiver ←→ ESP32 UART (9600 baud, UART_NUM_1)
                                        ↕
                              MQTT / WebSocket / Serial CLI
```

- LIN @ 9600 baud.
- Master: TruMinus (ESP32-P4). Slave: Truma Combi D.
- TruMinus emulates a CP Plus D (the original Truma wall control panel).
- LIN UART pins live in `main/main.cpp`. They are still being finalised on the JC4880-P4 — always grep that file for the current TX/RX assignment. The previous C5 board used TX=GPIO5 / RX=GPIO4 (P5 LP-UART), which is NOT applicable here.

### Bus cycle (~100 ms per iteration)

For each `linBusTask` iteration:
1. Prepare write frames with the current setpoints.
2. Read `FRAMES_TO_READ` slave frames (0x21, 0x22).
3. Write `FRAMES_TO_WRITE` master frames (0x02–0x07, 0x20).
4. Send one master frame via the LIN transport (0x3C → 0x3D), alternating between TOnOff (SID 0xB8) and TGetErrorInfo (SID 0xB2).

### Protected ID (LIN 2.x)
```
p0 = bit0 ^ bit1 ^ bit2 ^ bit4
p1 = ~(bit1 ^ bit3 ^ bit4 ^ bit5)
PID = (p1<<7) | (p0<<6) | (frameID & 0x3F)
```

---

## READ frames (Truma → TruMinus)

### Frame 0x21 — Combi_Info_1 (main CP Plus D status)

Primary status frame; replaces the 0x16 of the plain (non-D) CP Plus protocol. Length 8 bytes.

| Byte | Bits | Field | Encoding |
|------|------|-------|----------|
| 0    | 7:0  | `R_Room_Temperature_current` bits 7:0 | LSB of 12-bit Kelvin×10 |
| 1    | 3:0  | `R_Room_Temperature_current` bits 11:8 | MSB nibble |
| 1    | 7:4  | **4-bit rolling counter** | 0→F→0, ≈8–9 s/step. NOT flags. |
| 2    | 7:0  | `R_Water_Temperature_current` bits 11:4 | High byte of 12-bit Kelvin×10 |
| 3    | —    | Unknown | Constant 0xB8 in captures |
| 7    | —    | Unknown | — |

**Flags in byte 1 (bit by bit):**
- bit0 → `/antifreeze`
- bit1 → `/supply220` (220 V mains present)
- bit2 → `/window` (window open)
- bit3 → `/roomdemand` (heating demand active)
- bit4 → **IGNORE** — it's the LSB of the rolling counter, NOT `/waterdemand`
- bit7 → `/error` (error active)

**Temperature decode (both temperatures use the same encoding):**
```cpp
uint16_t rawRoom  = (uint16_t)fdata[0] | ((uint16_t)(fdata[1] & 0x0F) << 8);
uint16_t rawWater = (uint16_t)(fdata[1] >> 4) | ((uint16_t)fdata[2] << 4);
double roomTemp   = rawRoom  / 10.0 - 273.0;
double waterTemp  = rawWater / 10.0 - 273.0;
```
- `RawKelvinToTemp()` in `trumaframes.cpp` centralises this conversion.
- Both temperatures are sanity-checked (room: 0-50 °C, water: 0-100 °C) before being published.
- Water temperature here is the **authoritative source** for `truma/status/water_temp`; `TFrame22` only carries the `water_heating` state.

**Counter pitfall (byte 1 bits 7:4):** the rolling counter goes from ≈25 s/step to ≈8 s/step when the unit enters normal operation. It signals state transition; do NOT treat it as flags.

---

### Frame 0x22 — Boiler state (hot water)

| Byte | Description |
|------|-------------|
| 0    | State/counter (oscillates 0x81 ↔ 0x82 during post-run blower) |
| 1    | Burner state — bits 7:6 (see table) |
| 2    | Primary circuit temperature in **direct °C** (NOT Kelvin) — see note |

**Byte 1 — FWaterHeating:**
```cpp
FWaterHeating = (fdata[1] & 0xC0) == 0x40;
```
| Byte 1 (bits 7:6) | Meaning |
|--------------------|---------|
| 0x00               | Burner off |
| 0x40               | Burner active (heating) |
| 0x50               | Burner active (variant) |
| 0xD0               | Idle — setpoint active but temperature reached |

**Byte 2 — primary circuit (heat exchanger) temperature:**
- Direct °C (NOT Kelvin, NOT K×10).
- `0x10` = sentinel for "boiler fully off / no demand" — appears immediately after manual shutdown.
- ⚠️ **This is NOT the domestic hot water temperature.** It's the heat-exchanger primary loop return:
  - ≈48–49 °C = circuit active (constant regardless of 40 °C or 60 °C setpoint).
  - Decays gradually on automatic shutdown: 0x31 → 0x30 → … → 0x10.
- Valid range: 1–100 °C. Out of range or sentinel `0x10` → discard.

**Note:** since the current firmware, `truma/status/water_temp` is published from frame **0x21** (12-bit Kelvin×10 starting at bit 12), which reflects the actual domestic hot water temperature. `TFrame22` only publishes `water_heating`.

---

### Frame 0x16 — CP Plus status (legacy, non-D protocol)

Only active when 0x21/0x22 are NOT in use. Structure `frame16Data`:

| Field | Type | Description |
|-------|------|-------------|
| Antifreeze | bool | Antifreeze |
| Supply220 | bool | 220 V mains present |
| Window | bool | Window open |
| RoomDemand | bool | Heating demand |
| WaterDemand | bool | Hot water demand |
| Error | bool | Error active |
| RoomTemperature | uint16 | Kelvin×10 little-endian |
| WaterTemperature | uint16 | Kelvin×10 little-endian |
| BatteryVoltage | uint16 | raw/100 − 327.67 V |

---

### Diagnostic frames (optional)

Only available after enabling them via `TAssignFrameRanges`. Not used by the standard CP Plus D protocol (omitted to avoid corrupting the Truma's frame table).

| Frame | Main content |
|-------|--------------|
| 0x34  | Operating time (minutes), relays K1/K2/K3, EBT mode |
| 0x37  | Hydronic trend value (flame temperature, polynomial encoding) |
| 0x39  | Blower air temperature (×0.1 °C), flame temperature (polynomial), pump frequency (raw/25.0 Hz) |
| 0x35  | Burner blower voltage (×0.1 V), Hydronic burner state, glow plug state |
| 0x3b  | Battery voltage (×0.1 V), exhaust blower RPM (58594/raw), Hydronic error, circulation motor current (×0.1 A) |

**Flame temperature (frames 0x37, 0x39) — polynomial encoding:**
```
temp = raw³ × 1.8602e-5 + raw² × (-4.895e-4) + raw × 1.4471 - 65.647
```

**Exhaust blower RPM (frame 0x3b):**
```
rpm = 58594.0 / raw    (if rpm < 260 → publish 0)
```

---

## WRITE frames (TruMinus → Truma)

All of them are sent on every bus cycle (~100 ms).

### Frame 0x20 — Main CP Plus D control ⭐

Primary control frame. **This is the one the Truma actually obeys** in CP Plus D mode.

| Byte | Standby | Description |
|------|---------|-------------|
| 0    | 0xAA    | Room setpoint K×10, bits 7:0 (LSB of 12-bit) |
| 1    | 0xAA    | Room setpoint K×10 bits 11:8 in low nibble + 0xA0 in high nibble |
| 2    | 0xAA    | Water setpoint = K×10 >> 4 (bits 11:4) |
| 3    | 0xFA    | Constant |
| 4    | 0x00    | Constant |
| 5    | 0x00    | Packed nibbles: high=fan/heat mode, low=water mode |
| 6    | 0xE0    | Constant |
| 7    | 0x0F    | Constant |

**Bytes 0–1 — room setpoint:**
- Heating off: `0xAA 0xAA`.
- Heating on: `raw = (°C + 273.0) × 10.0`; byte0 = raw & 0xFF; byte1 = 0xA0 | (raw >> 8 & 0x0F).
- Example 20 °C → raw = 2930 = 0x0B72 → byte0 = 0x72, byte1 = 0xAB.

**Byte 2 — water setpoint (K×10 >> 4):**
- Water off: `0xAA`.
- Formula: `raw = (°C + 273.0) × 10.0; byte2 = raw >> 4`.
- 40 °C → raw = 3130 = 0x0C3A → byte2 = 0xC3.
- 60 °C → raw = 3330 = 0x0D02 → byte2 = 0xD0.

**Byte 5 — high nibble (fan / heating mode):**
| Nibble | Meaning |
|--------|---------|
| 0x0    | Fan OFF |
| 0x1–0x9 | Fan speed 1–9 |
| 0xA    | Fan speed **10 (max)** |
| 0xB    | Heating eco |
| 0xD    | Heating high |

⚠️ **0xA pitfall:** the real CP Plus sends 0xA0 in standby (all 0xAA), which looks like "nibble = off". But in active mode, nibble = 0xA means speed 10, NOT off. Using 0xA as "off" makes the Truma spin the fan at maximum.

**Byte 5 — low nibble (water mode):**
- 0 = water off.
- 1 = water active (eco/high/boost).

**Common patterns:**
```
All off:                AA AA AA FA 00 00 E0 0F
Heat eco + water eco:   SP_L SP_H C3 FA 00 B1 E0 0F
Heat eco + water high:  SP_L SP_H D0 FA 00 B1 E0 0F
Water 60 °C only:       AA AA D0 FA 00 01 E0 0F
Water 60 °C + fan sp.5: AA AA D0 FA 00 51 E0 0F
```

---

### Frames 0x02, 0x03, 0x04 — TFrameSetTemp (legacy)

Kelvin×10 little-endian encoding (same for all three):
```cpp
rawvalue = htole16((uint16_t)((temp + 273.0) * 10));
fdata[0..1] = rawvalue;
```
- 0x02: simulated room temperature (debug).
- 0x03: room setpoint.
- 0x04: water setpoint.

---

### Frame 0x05 — TFrameEnergySelect

| Index | Mode | Priority (byte 0) |
|-------|------|-------------------|
| 0 = EsGasDiesel | Gas/Diesel | EpFuel (1) |
| 1 = EsMixed900  | Gas + Elec 850 W | EpBothPrioFuel (3) |
| 2 = EsMixed800  | Gas + Elec 1700 W | EpBothPrioFuel (3) |
| 3 = EsElectro900 | Elec 850 W | EpBothPrioElectro (2) |
| 4 = EsElectro1800 | Elec 1700 W | EpBothPrioElectro (2) |

---

### Frame 0x06 — TFrameSetPowerLimit

Limit in watts, little-endian (bytes 0-1):
`limits[] = {0, 900, 1800, 900, 1800}` for indices 0–4.

---

### Frame 0x07 — TFrameSetFan (legacy)

```
byte0 = PumpOrFan | 0xE0
byte1 = 0xFE
```
PumpOrFan: 0x10 = off, 0x11 = eco, 0x12 = high, 0x10 | N = speed N (1–10), 1 = eco heat, 2 = high heat.

---

## Master Frames (LIN transport 0x3C / 0x3D)

A master frame is sent via PID 0x3C (request) and the answer is read back via 0x3D.
The firmware alternates between TOnOff and TGetErrorInfo on every cycle.

### TOnOff (SID 0xB8) — on/off

**Request** (sent as `fdata` of frame 0x3C):
```
byte[0] = 0x01  (NAD)
byte[1] = 0x06  (LEN — single frame)
byte[2] = 0xB8  (SID)
byte[3] = 0x20
byte[4] = 0x03
byte[5] = 0x00 (off) | 0x01 (on)
byte[6] = 0x00
```

**Response** (read from 0x3D):
```
byte[2] == 0xF8  (SID + 0x40 = 0xB8 + 0x40) — checkReply
byte[3] = requested_state
byte[4] = current_state
```

**States:**
| Value | Meaning |
|-------|---------|
| 0     | No tin (no power) |
| 1     | Idle (off, ready to receive) |
| 2     | On (running) |
| 3     | Shutdown (powering down) |
| 4     | Powering up (starting) |

---

### TGetErrorInfo (SID 0xB2) — error info

**Request:**
```
byte[0] = 0x7F  (NAD)
byte[1] = 0x06  (LEN)
byte[2] = 0xB2  (SID)
byte[3] = 0x23
byte[4] = 0x17
byte[5] = 0x46
byte[6] = 0x20
byte[7] = 0x03
```

**Response:**
```
byte[2] == 0xF2  (SID + 0x40) — checkReply
byte[4] = errorClass
byte[5] = errorCode
byte[6] = errorShort
```

**Error classes:**
| Class | Severity | Action |
|-------|----------|--------|
| 0x00  | No error | — |
| 0x01, 0x02 | Warning | Informational; no action required |
| 0x05  | Ignition failure | Normal on cold start; the Truma auto-retries |
| 0x06  | Hard lockout | Physical 12 V power-cycle ~30 s required |
| 0x10, 0x20, 0x30 | Error | Software reset may clear it |
| 0x40  | Locked | Requires technical support or 12 V power-cycle |

---

## Error reset sequence

```
1. HandleCommandReset()
2. truma_reset = true — only TOnOff is enabled
3. linBusTask → onOff->SetOn(false) on every cycle
4. Wait for TOnOff response: currentState == 1 (idle)
5. When state == 1: stop comms for 10 s (truma_reset_stop_comm = true)
6. After 10 s: resume normal comms
7. Timeout: if state == 1 is not seen within 120 s → cancel reset
```

**Important:** software reset (the procedure above) only works for class ≤ 0x05 errors. Class 0x06 always requires a physical 12 V power cycle.

---

## Operation logic (main loop / linBusTask)

### PumpOrFan computation

```
If !heating:
  fanMode > 0    → PumpOrFan = 0x10 | fanLevel (speed 1–10)
  fanMode == -2  → PumpOrFan = 0x12 (high, no heat)
  fanMode == -1  → PumpOrFan = 0x11 (eco, no heat)
  fanMode == 0   → PumpOrFan = 0x10 (off)

If heating:
  LocSetPointTemp = roomSetpoint
  Force fanMode to eco (-1) or high (-2)
  fanMode == -2  → PumpOrFan = 2 (high heat)
  fanMode == -1  → PumpOrFan = 1 (eco heat)
```

### WaterBoost

Activated when `boiler == "boost"` (distinct from "high" even though both are 60 °C):

1. `TWaterBoost::Start()` — starts a 40-minute timer.
2. While boosting: `LocSetPointTemp = 0`, `PumpOrFan = 0` → pauses heating to maximise water heating throughput.
3. Auto-stops when:
   - 40 minutes have elapsed, OR
   - `waterHeating` drops false after having been true (target reached).
4. On stop: automatically switches boiler to "high" (`fstoppayload`).

### Truma on/off decision

```
ON if: heating || waterSetpoint > 0 || fanMode != 0 || forceon
OFF: after 20 s of inactivity (off_delay)
During reset: forced OFF
```

---

## Fan and boiler modes

### Fan (TFanSetting)

| String value | fintvalue | Description |
|--------------|-----------|-------------|
| "off"        | 0         | Fan off |
| "eco"        | −1        | Eco (heating on) |
| "high"       | −2        | High (heating on) |
| "1"–"10"     | 1–10      | Numeric speed (heating off) |

### Boiler (TBoilerSetting)

| String value | Setpoint °C | Description |
|--------------|-------------|-------------|
| "off"        | 0.0         | Boiler off |
| "eco"        | 40.0        | Hot water 40 °C |
| "high"       | 60.0        | Hot water 60 °C |
| "boost"      | 60.0        | 40-min boost (heating paused) |

---

## MQTT topics

| Topic | Direction | Values | Description |
|-------|-----------|--------|-------------|
| `truma/status/room_temp` | ← | float | Room temperature (°C) |
| `truma/status/water_temp` | ← | float | Hot water temperature (°C) |
| `truma/status/water_heating` | ← | 0/1 | Burner active |
| `truma/status/roomdemand` | ← | 0/1 | Heating demand |
| `truma/status/waterdemand` | ← | 0/1 | Water demand (frame 0x16, legacy) |
| `truma/status/linok` | ← | 0/1 | LIN bus OK |
| `truma/status/err_class` | ← | int | Error class (0 = OK) |
| `truma/status/err_code` | ← | int | Error code |
| `truma/status/requested_state` | ← | 0–4 | State requested from the Truma |
| `truma/status/current_state` | ← | 0–4 | Current Truma state |
| `truma/set/temp` | → | 5.0–30.0 | Room temperature setpoint |
| `truma/set/heating` | → | 0/1 | Heating on/off |
| `truma/set/boiler` | → | off/eco/high/boost | Boiler mode |
| `truma/set/fan` | → | off/eco/high/1–10 | Fan mode |
| `truma/set/energy_idx` | → | 0–4 | Energy source index |
| `truma/set/error_reset` | → | 1 | Start error reset |
| `truma/set/ping` | → | any | Keep Truma awake |
| `truma/set/refresh` | → | any | Force republish of all values |
| `truma/set/simultemp` | → | −273–30 | Simulated room temperature (debug) |

---

## Normal behaviour vs. noise

### Start-up interference
During burner ignition, the fuel pump and blower generate LIN-bus glitches:
- Frame 0x21: rolling counter goes backwards, bytes out of range.
- Cadence: ≈523 ms between glitches.
- They disappear once the burner stabilises.

### Post-run blower
After an automatic temperature shutdown:
- Frame 0x22 byte 0 oscillates 0x81 ↔ 0x82 for several minutes.
- It's the blower cooling the combustion chamber.
- Completely normal.

### Water demand vs. temperature
`waterHeating` (frame 0x22 byte 1) may stay active for a moment after the setpoint drops and the temperature is already satisfied — protocol/cooldown delay. The UI uses the circuit temperature (`waterTemp >= setpoint − 1 °C`) to force "at target" state independently of the flag.

---

## Diagnostic tools

**Serial CLI:**
```
sniff on|off      passive LIN bus listen
busindex          full bus state (frames + raw bytes)
lindebug on|off   verbose mode for the LIN driver
reset             start error reset
boiler eco|high|boost|off
heating 0|1
fan off|eco|high|1..10
temp 5.0..30.0
```

**Output of `busindex`:**
```
RX 21h : OK  XX XX XX XX XX XX XX XX  room:XX.X°
RX 22h : OK  XX XX XX XX XX XX XX XX  water:XX.X° heat:0|1
TX 20h :     XX XX XX XX XX XX XX XX  room_sp:XX.X° fan/water:XXh
B8 onOff: OK  req=X cur=X  (on|off)
B2 errInfo: OK  class:XXh code:XXh
```
