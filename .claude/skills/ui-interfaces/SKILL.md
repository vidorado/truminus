# TruMinus — User Interfaces

High-level context on the two user interfaces and how they stay in sync. No detailed layout or LVGL code here — see CLAUDE.md for that.

---

## Both interfaces are functionally equivalent

Every control feature must exist in both:
- **LCD touch UI** (`main/p4display.cpp`) — physical 800×480 LVGL screen on the JC4880-P4 board (replaces the old 320×240 CYD-C5 implementation).
- **Web** (`data/index.html` + `script.js` + `styles.css` + `i18n.js`) — WebSocket JSON, no framework, responsive.

If a control, status indicator, or piece of UI logic is added on one side, the equivalent must land on the other. The only exception is when the user explicitly asks for a single-surface change.

---

## Source of truth

The setting objects in `settings.cpp` (`TTempSetting`, `TBoilerSetting`, `TFanSetting`, `TOnOffSetting`) are the single shared source of truth. Both the LCD and the web read/write these objects; the Truma receives the values on the next LIN bus cycle.

Energy selection (`s_energyIdx` in `main.cpp`) is the one exception: a volatile shared integer rather than a `TMqttSetting`.

---

## WebSocket protocol

The ESP32 acts as a WebSocket server. The browser uses `ReconnectingWebSocket` for auto-reconnect.

**Server → client messages:**
```json
{"command": "setting", "id": "/field", "value": "..."}   ← controllable settings
{"command": "status",  "id": "field",  "value": "..."}   ← Truma readings
{"command": "solar",   "id": "...",    "value": "..."}   ← solar data (every 10 s)
{"command": "batt",    "id": "...",    "value": "..."}   ← battery data (every 10 s)
```
- `setting` ids start with `/`; `status` ids do not.
- Sent on connect and whenever the value changes.
- Solar/battery are published every 10 s by `publishSolarBatt()` in `main.cpp`.

**Client → server:**
```json
{"id": "/field", "value": "..."}
```
No `command` field. The `id` includes the leading slash.

**Handshake on connect:** the client sends the literal string `"settings"`; the server responds with all current settings (temp, heating, boiler, fan, energy_idx, outdoor_temp). Truma status fields (room_temp, water_temp, etc.) arrive on the next LIN cycle via `doforcesend`.

**Keepalive:** the client sends `"ping"` every 10 s so the ESP32 doesn't shut the Truma off for inactivity.

---

## Settings vs. Status

| Category | Fields | Direction | Persisted |
|----------|--------|-----------|-----------|
| Settings | temp, heating, boiler, fan, energy_idx | bidirectional | LCD: NVS; Web: no |
| Status | room_temp, water_temp, water_heating, linok, err_class, err_code, outdoor_temp | server → client only | no |
| Solar/Batt | solar_state, solar_voltage, solar_load, solar_yield, batt_soc | server → client only | no |

The web persists nothing: on reload or reconnect it re-fetches state from the ESP32 via the handshake and the first LIN cycle. NVS persistence is handled by the LCD side.

---

## energy_idx synchronisation

Energy mode is the only setting with a special sync path (it doesn't go through `TMqttSetting`):
- **LCD → ESP32 → Web:** `main.cpp` detects a change in `p4GetEnergyMode()` and broadcasts `{"command":"setting","id":"/energy_idx",...}`.
- **Web → ESP32 → LCD:** `handleSetting()` calls `p4SetEnergyIdx(idx)` under the LVGL mutex.

> The function names above (`p4GetEnergyMode` / `p4SetEnergyIdx`) follow the new `p4display.*` API. The previous board used `cydGetEnergyMode` / `cydSetEnergyIdx`; treat any reference to those as a porting TODO.

---

## Behaviour differences between interfaces

### Fan disabled while boiler is active (LCD only)
The fan On/Off buttons sit in `LV_STATE_DISABLED` while the boiler is running. The web doesn't enforce this — a fan command issued from the web while the boiler is on is queued and applied when the boiler stops.

### Debounce (web only)
All web controls send with a 300 ms debounce to avoid flooding the ESP32 with rapid taps. The LCD sends on every tap, no debounce.

### Force fan to off when boiler activates (LCD only)
`boilerCb` forces `fan="off"` when the boiler is enabled from the LCD. `main.cpp` immediately overrides this back to "eco" if heating is on. It's a benign side effect that doesn't exist on the web path.

### Outdoor temperature
The LCD has an AM2301/DHT22 sensor (pin TBD on the JC4880-P4 — confirm in `main/main.cpp`; on the previous C5 board it was GPIO17). Outdoor temperature is shown in the LCD top bar and broadcast to web clients as `{"command":"status","id":"outdoor_temp","value":"..."}`. Web clients display it but cannot read it independently.

---

## Status indicators (tint and fire)

Both interfaces use the same three-state logic:

| State | Tint (drop, water) | Fire (flame, heating) |
|-------|--------------------|------------------------|
| Off | dim blue (LV_OPA_20) | dim blue (LV_OPA_20) |
| Active, at target | solid blue | solid orange |
| Active, heating up | blinking blue | blinking orange |

The "heating → at target" transition is computed from direct temperature comparison against the setpoint, NOT from the protocol flag — the Truma keeps the flag asserted during post-run cooldown. Thresholds: `waterTemp >= setpoint − 1 °C`, `roomTemp < setpoint − 0.3 °C` for heating.

---

## Solar and battery panels

### LCD (JC4880-P4)
Bottom-right of the content area (under the hot-water section). Occupies the full right column width (429 px) and splits into:
- **Left column:** four solar data lines — Status (no label), Volt:, Load:, Yield: (label fonts subject to final layout in `p4display.cpp`).
- **Vertical separator** (1 px).
- **Right column:** SOC % label + vertical battery icon with proportional fill.

### Web
Four equal panels in landscape (`grid-template-columns: 1fr 1fr 1fr 1fr`), 2×2 in portrait. The solar panel shows:
- Status (white)
- Voltage (white)
- Load A + W (light blue)
- Daily yield kWh (yellow)

The battery panel shows SOC % + an animated vertical battery icon.

---

## Truma errors

Same behaviour on both UIs:
1. **Status bar:** error class + code with severity colour (amber=warning, red=error, dark red=locked).
2. **Modal:** auto-opens on a new error; explicit dismiss required.
3. **Error reset:** only available from the serial CLI or MQTT (`truma/set/error_reset=1`). By design, neither GUI exposes a reset button — it's an infrequent, risky operation.

---

## LCD settings screen (⚙ button)

Reachable from the gear icon in the top bar. Opens a menu with five options:
- **WiFi Config** — blocking screen for SSID + password; saves to NVS and reboots.
- **MQTT Config** — blocking screen for host, port, user, password; saves to NVS and reboots. MQTT is optional (web/WebSocket alone works).
- **Solar charge** — Victron MAC (12 hex) + encryption key (32 hex) + Ultimatron battery MAC (12 hex). Works even when BLE is not compiled in (allows pre-provisioning). Saves to NVS.
- **Display** — screen-off timeout selector: 30 s, 1 min, 3 min, or never. Saved to NVS immediately on tap.
- **Language** — Spanish / English. Applied immediately.

The timeout has three stages: full brightness → dimmed warning (last 5 s) → screen off. The first tap while off wakes the screen without firing the underlying control.

On first boot without saved credentials, the WiFi (and optionally MQTT) screen is launched automatically before the main UI is shown.

---

## What the web does NOT have

- WiFi / MQTT / Solar configuration (LCD only, via settings screen).
- Touch calibration (LCD only; on GT911 the controller is factory-calibrated so this may be a no-op on JC4880-P4).
- Brightness / screen-timeout control (LCD only).
- Error reset button (neither UI has it — CLI or MQTT only).

## LVGL scan overlay pattern (p4settings.cpp)

Scan screens (WiFi AP list, Victron BLE list, battery BLE list) are implemented
as proper LVGL screens, not as child widgets of the existing settings screen:

```cpp
lv_obj_t* ov = lv_obj_create(nullptr);   // nullptr → new LVGL screen
lv_obj_set_user_data(ov, prev_screen);   // store return target
lv_screen_load(ov);                       // make it active
```

**Why not a child widget?** `lv_obj_create(parent)` with full-screen size has
z-order/rendering issues in LVGL 9 — the overlay may not paint at all over the
parent screen. The proper LVGL 9 way is a real screen.

**Back navigation:** all close paths (cancel button, item selection) do:
```cpp
lv_obj_t* prev = (lv_obj_t*)lv_obj_get_user_data(ov);
lv_screen_load(prev);
lv_obj_delete(ov);
```

The spinner goes in the title bar (not in the flex-column list) because LVGL
flex layout overrides `lv_obj_center()`.

---

## What the LCD does NOT have

- Change history (the screen shows current state only).
- Concurrent multi-user access (one physical user at a time).

---

## Web asset build

Files under `data/` are NOT served from LittleFS at runtime. `scripts/compress_fs.py`, invoked at cmake configure time by the root `CMakeLists.txt`, gzip-compresses each file into a `static const uint8_t` array inside `main/webfiles.h`. The firmware serves them straight from embedded flash.

**Implications:**
- Any change to `data/` requires a **firmware rebuild and reflash** (`pio run --target upload`). There is no `uploadfs` target on this project.
- Assets are served with `Cache-Control: max-age=31536000, immutable` to reduce ESP32 load.
- Individual files larger than ~64 KB after gzip will break the build (C array + compression limit).
