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

> ⚠️ **The `snapshot` JSON must fit `WS_QUEUE_MSG` (webserver.cpp).** Every WS frame is copied through a fixed-size FreeRTOS queue slot; `wsQueueSend()` **drops** an over-length message (it used to silently truncate → invalid JSON). The connect `snapshot` is by far the largest frame (all settings + 32-char SSID + IP), so **adding a setting field to the snapshot can push it over the cap.** A dropped/clipped snapshot never reaches the client's `d.command==='snapshot'` branch, so `hideReloadVeil()` never runs and the web sits on the "Conectando…" veil forever while later broadcasts still update the DOM underneath (the classic symptom). When you add a snapshot field, re-check its worst-case length against `WS_QUEUE_MSG` (currently 320 B; snapshot ~284 B worst case) and bump the cap if needed (keep `WS_QUEUE_LEN*WS_QUEUE_MSG` near-flat to spare internal DRAM — the OTA self-test heap floor watches that window).

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
The LCD has an AM2301/DHT22 sensor on **GPIO52** (`AM2301_DATA_PIN` in `main/main.cpp`), read every 30 s via the RMT peripheral (`main/am2301.cpp`). Outdoor temperature is shown in the LCD top bar and broadcast to web clients as `{"command":"status","id":"outdoor_temp","value":"..."}`. Web clients display it but cannot read it independently.

### LVGL lock for `p4display.cpp::st` mutations
The remote setters (`p4SetHeating`, `p4SetFanMode`, `p4SetBoilerMode`, `p4SetEnergyIdx`, `p4SetRoomSetpoint`, `p4SetScreenTimeoutIdx`) take `bsp_display_lock(50)`; the on-screen button callbacks already run on the LVGL task with the lock held. The diff-broadcast helper (`broadcastControlChanges` in `main.cpp`) reads `st` via `p4GetControlState` from `wsPumpTask` *without* the lock — safe because each field is a plain int/bool/float, but don't introduce composite reads. Prefer a short lock timeout (e.g. 10 ms) over `portMAX_DELAY` to avoid deadlocks if the LVGL task is busy.

### p4GetControlState checklist
**Every** field in `P4ControlState` (control_state.hpp) must have a matching `out.field = st.field` line in `p4GetControlState()`. Missing one silently breaks LCD→Web sync for that field: `broadcastControlChanges` always reads 0/false and never emits the change, while Web→LCD still works because `p4Set*` writes directly to `st`. When adding a new field to `P4ControlState`, update `p4GetControlState`, `broadcastControlChanges`, the `onWsConnected` snapshot JSON, and the web's `applySetting` handler.

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

## Row 2 panels: FAN / SOLAR / BATERÍA / INVERSOR

### SOLAR (compact)
Status line + charge (A/W, no label) + yield (kWh, no label).  No voltage line.

### BATERÍA (standalone panel)
SOC % + vertical battery icon + horizontal flow line + CARGA power port.
The port uses Ultimatron BMS data (`battV × battA`), NOT the Multiplus
inverter data.  Port color: green when charging (`← CARGA`), red when
discharging (`DESCARGA →`), grey at idle.

### INVERSOR (Multiplus VE.Bus)
Three I/O ports (RED / CARGAS / BAT.) flanking a Multiplus body icon.
Dynamic port coloring: RED → green + plug-circle-bolt icon when AC
connected; CARGAS → red when delivering power; BAT. → green (charging,
right arrow) / red (discharging, left arrow).  Flow lines animate with
a striped pattern moving in the direction of energy flow.

### LCD (JC4880-P4)
Same four panels with matching dynamic colors (`C_PORT_GREEN_*`,
`C_PORT_RED_*`, `C_PORT_GREY_*`).  `P4MultiData.acInState` drives
the RED port color; `make_io_box` returns box/header/label refs for
runtime color changes.

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

Files under `data/` are served from LittleFS (see CLAUDE.md §"Web assets
are served from LittleFS").  `cache_bust.py` rewrites `?v=<sha1>` query
strings in `index.html` on every build so browsers refetch changed assets.
**When editing CSS/JS locally** (opening `data/index.html` directly in a
browser), the `?v=` hash is stale — force a hard refresh (Ctrl+Shift+R)
or the browser will show cached styles.

---

## CSS layout gotchas (hard-won lessons)

### Column widths
- Row 2 is a 4-column grid: `grid-template-columns: 22% 16% 22% 40%` (base).
  Landscape overrides to `20% 16% 23% 41%`.  Portrait uses a 10-column
  `.lcd` grid with `display: contents` on `.row` wrappers.
- When changing column widths, always check all three breakpoints (base,
  portrait ≤460px, landscape ≤500px height).

### Portrait layout (≤460px)
- Body is `display: flex; flex-direction: column; height: 100dvh`.
  Topbar and statusbar are `position: static !important` (not fixed) so
  they participate in the flex.  `.lcd` uses `flex: 1 1 0px` +
  `min-height: 0 !important` to take the remaining space.
- The grid uses `grid-template-rows: minmax(0, Xfr)` to distribute
  height proportionally.  `minmax(0, ...)` is required — plain `Xfr`
  won't shrink below content size and causes overflow.
- `body { min-height: calc(100dvh + 1px) }` from the base must be
  overridden with `!important` in portrait or it prevents flex shrinking.

### Specificity traps
- Rules inside `@media (max-width: 460px)` with `.panel-water { ... }`
  lose to later base rules like `.panel-water { display: grid; ... }`.
  Use `.row .panel-water` for higher specificity inside the media query.
- Same applies to `.panel-batt`, `.panel-solar` — prefix with `.row`
  when overriding base styles from within a media query.

### INVERSOR flow lines
- Flow lines are `position: absolute` inside `.inv-mid`.  Their `width`
  must match the horizontal `padding` of `.inv-mid` exactly, otherwise
  they disconnect from the ports.
- In landscape, where columns are narrower (`0.4fr`), use
  `left`/`right` + `calc(50% + half-mpx-width)` with `width: auto`
  instead of fixed pixel widths — this makes them scale with the column.
- The animated zebra stripe uses `repeating-linear-gradient` +
  `background-position` animation.  `background-size` must match the
  element height (e.g. `8px 6px` for a 6px-tall line).

### Font icon subsets
- Web icons: `data/fa-solid.woff` — regenerate with
  `python3 scripts/gen_fa_subset.py` after adding glyphs.
- LCD icons: `main/font_icons.ttf` — regenerate with
  `python3 scripts/gen_icon_font.py`.
- Both must be updated in tandem when adding new icons.
- Define `#define FA_xxx` macros in `p4display.cpp` using the UTF-8
  encoding of the codepoint.
