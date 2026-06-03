# TruMinus Refactoring Plan

## Scope and philosophy

TruMinus is **mid-migration** (ESP32-C5 → JC4880P443C / ESP32-P4) with several legacy
modules still dormant (`settings.cpp`, `trumaframes.cpp`, `waterboost.cpp`,
`autodiscovery.cpp`, `commandreader.cpp`) waiting to be ported to bring MQTT, Home
Assistant autodiscovery, water-boost and the serial CLI back online.

Because of that, this plan is deliberately **narrow**:

1. **Test pure logic, then extract it.** Anything that can run on the host build
   (`test/host/`, Catch2) is fair game: protocol encoding, parsers, mode derivation,
   diff/formatting helpers.
2. **Leave hardware/framework-coupled code alone.** LVGL UI, BLE stacks, the WSS
   tunnel, the HTTP server, WiFi/C6 bring-up and OTA flashing cannot be meaningfully
   unit-tested on host. Refactoring them now is high-risk, low-reward churn.
3. **Do not re-architect global state before the migration finishes.** Reworking the
   global state model, error-handling style or config access while dormant modules are
   still being reintegrated optimizes a moving target.

> **Rejected from the previous plan (over-engineering for this codebase):** a
> `StateManager` with observers, a `Result<T,E>` error type, a `ConfigManager` NVS
> facade, generic "error types", and test suites for UI view-models / color math.
> These are desktop/server patterns that add indirection (RAM/flash, runtime cost) on a
> real-time MCU without a concrete payoff here. Revisit only if a real need appears
> after the migration.

---

## Host-test fidelity rule (IMPORTANT)

A module is only genuinely covered if the host test **links the real `main/*.cpp`**,
not a hand-copied re-implementation. The original suite copied logic into
`test/host/test_helpers.cpp` / inline in the test files, so the tests could stay green
while production drifted (e.g. `derive_mode`'s real signature returns a struct while the
test copy used out-params).

To link real code on the host, a logic module's headers must be **free of
ESP-IDF / LVGL / FreeRTOS includes**. Plain data structs shared with the display/LIN
layers live in `main/control_state.hpp` (IDF-free); `p4display.hpp` includes it. Any new
testable module must follow the same rule: depend only on IDF-free headers, then add its
`.cpp` to `truminus_logic` in `test/host/CMakeLists.txt`.

**Still copied (pending the same treatment):** `test_helpers.cpp` keeps copies of the
LIN frame encode/parse, BTHome and semver logic, because their real sources
(`truma_lin.cpp`, `tankble.cpp`, `p4_ota.cpp`) still pull in IDF headers. Migrate as
those modules get their pure logic peeled into IDF-free units.

## Current state

- **Host test suite:** 363 assertions across 46 `TEST_CASE`s (Catch2 v3.5.2),
  built via `test/host/build.sh`, run with `ctest` or `./build/tests`.
- **Covered by tests today:** LIN encoding/frames/protocol, `derive_mode`, fan/boiler
  conversions, AM2301 decode, BTHome (tank) parse, Multiplus parser + bitreader,
  semver compare, fault log.
- **`mode_controller`** ✅ consolidated into a single header (`mode_controller.hpp`;
  the stray `mode_controller.h` is gone) and now depends on `control_state.hpp`. Its
  tests (`test_fan_boiler.cpp`, `test_derive_mode.cpp`) link the **real**
  `main/mode_controller.cpp` — first module on the fidelity rule above. Firmware build
  confirmed green after the decoupling.

| Source file        | Lines |
|--------------------|-------|
| `p4display.cpp`    | 2138  |
| `wstunnel.cpp`     | 1008  |
| `main.cpp`         | 841   |
| `p4_ota.cpp`       | 807   |
| `victronble.cpp`   | 619   |
| `webserver.cpp`    | 568   |
| `truma_lin.cpp`    | 432   |
| `multiplusble.cpp` | 335   |
| `i18n.cpp`         | 321   |
| `cli.cpp`          | 306   |
| `ultimatronble.cpp`| 292   |
| `am2301.cpp`       | 246   |
| `tankble.cpp`      | 228   |
| `faultlog.cpp`     | 84    |

---

## Work items (in priority order)

### 0. Consolidate `mode_controller` headers ✅ DONE
Merged into a single `mode_controller.hpp` (depending on the new IDF-free
`control_state.hpp`); `mode_controller.h` removed; `main.cpp`/`truma_lin.cpp` updated.
Tests migrated to link the real `.cpp`. Firmware + host tests green.

### 1. WebSocket command parsing — *high value, testable* — NEXT
Follow the fidelity rule: extract the pure id+value → validated action mapping out of
`onWsCommand()` (`main.cpp`) into an IDF-free `ws_command.{hpp,cpp}`, link it into the
host tests, then add `test_ws_command.cpp` BEFORE rewiring `main.cpp` to dispatch via it.
`onWsCommand()` (`main.cpp:74`) maps incoming `{id,value}` frames onto control-state
mutations. Extract the **pure** part (id+value → validated control change) into a
helper that takes/returns plain structs, leaving the queue/global writes in `main.cpp`.
- Tests: each writable id (`temp`, `heating`, `boiler`, `fan`, `energy_idx`, …),
  invalid ids, out-of-range values, malformed input.

### 2. Broadcast diff logic — *high value, testable*
`main.cpp` has eight `broadcast*` functions (`broadcastControlChanges`,
`broadcastNetInfoChange`, `broadcastBleData`, `broadcastMultiplusData`,
`broadcastTankData`, `broadcastLinTemps`, `broadcastIconStates`, `broadcastOtaStatus`)
that each implement "compare against last-sent, emit JSON on change". Extract the
**comparison + JSON-formatting** into pure helpers; keep `wsQueue` enqueueing in place.
Do **not** build a heavyweight templated `WsBroadcaster` class — small free functions
are enough and stay testable.
- Tests: change-detection per field, JSON formatting, no-op when unchanged.

### 3. Victron & Ultimatron BLE parsers — *medium, testable*
The Tank (BTHome), Multiplus and AM2301 parsers are already extracted and tested. The
two missing pure parsers are **Victron Instant Readout** (`victronble.cpp`) and
**Ultimatron GATT response** (`ultimatronble.cpp`). Extract their byte-decoding into
parser functions decoupled from the NimBLE callbacks.
- Tests: valid frames, invalid/short data, sentinel/out-of-range values, (Victron)
  decryption boundary cases.

### 4. OTA version/semver logic — *medium, partially testable*
Semver compare already has tests. If more pure logic can be peeled off `p4_ota.cpp`
(version-eligibility rules, auto-prompt minor/major policy) without touching the flash
state machine, extract and test it. The download/flash/rollback machinery stays as is.

---

## Explicitly deferred (until after migration / not at all)

- Global state encapsulation / observer pattern.
- `Result<T,E>` / unified error-handling rewrite.
- `ConfigManager` NVS facade.
- Display (LVGL) UI/state split and view-model tests.
- WSS tunnel, WiFi manager, HTTP server, p4settings refactors.
- CLI / i18n test suites (low ROI; revisit when the legacy CLI is reactivated).

---

## Conventions

- Pure-logic tests in `test/host/`, named `test_<module>_<feature>.cpp`, registered in
  `test/host/CMakeLists.txt`.
- Build + run: `bash test/host/build.sh && test/host/build/tests`.
- After each extraction, **rebuild the firmware** (`make`) to confirm nothing broke,
  and keep the host suite green.
- All repo artifacts (code, comments, docs, this file) are English-only per `CLAUDE.md`.

---

**Last updated:** 2026-06-04
**Supersedes:** the 20-area plan drafted by Qwen 3.7 Max (trimmed for scope and
corrected for the in-progress migration).
