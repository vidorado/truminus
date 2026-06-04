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

**Still copied (pending the same treatment):** the LIN low-level checksum/PID and the
frame-assembly setters are still reimplemented inline in `test_lin_protocol.cpp` /
`test_lin_frames.cpp` (their real sources `lin_driver.cpp` / `truma_lin.cpp` keep IDF
deps, and the setters mutate shared frame buffers). `test_helpers.cpp/.hpp` are now empty
shells — removable once `test_lin_protocol.cpp` drops its leftover include.

## Current state

- **Host test suite:** 394 assertions across 53 `TEST_CASE`s (Catch2 v3.5.2),
  built via `test/host/build.sh`, run with `ctest` or `./build/tests`.
- **IDF-free modules linked into the host tests (real production code, not copies):**
  `mode_controller` (fan/boiler + `derive_mode`), `version_compare` (semver),
  `lin_codec` (Kelvin temp encode/parse), `bthome_codec`, `am2301_codec`,
  `multiplus_codec` (BitReader + VE.Bus unpack), `ultimatron_codec`, `victron_codec`,
  `faultlog_codec`. Plain data structs they share live in IDF-free headers
  (`control_state.hpp`; the BLE `*Data` structs were already IDF-free).
- Each extraction verified: host suite green **and** firmware build green.

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

### BLE parsers + small codecs ✅ DONE
`bthome_codec`, `am2301_codec`, `multiplus_codec`, `ultimatron_codec`, `victron_codec`,
`version_compare`, `lin_codec`, `faultlog_codec` extracted as IDF-free modules; their
tests link the real code. New tests added for the previously-untested Ultimatron and
Victron parsers. Drift caught and fixed along the way (e.g. `MULTI_POWER_NA` was
`-999999` in the multiplus copy vs the real `INT32_MIN`; `derive_mode`'s copy used
out-params vs the real struct return). AES-CTR (mbedtls) and NimBLE/RMT plumbing stay in
their `*.cpp`; the codecs take already-decrypted / already-captured data.

### Remaining IDF-free extractions
- **LIN low-level (checksum/PID)** — peel `getProtectedId`/`getChecksum` out of
  `lin_driver.cpp` into an IDF-free unit; repoint `test_lin_protocol.cpp`.
- **LIN frame setters** (`f20_*`/`f05_*`/`f06_*`/`f07_*`) — these mutate shared frame
  buffers; need refactoring to take a buffer pointer before `test_lin_frames.cpp` can
  link the real code. Bigger change.
- **`test_helpers.{cpp,hpp}` removal** once the two LIN tests above stop including it.

### 1. WebSocket command parsing — *high value, testable*
Follow the fidelity rule: extract the pure id+value → validated action mapping out of
`onWsCommand()` (`main.cpp`) into an IDF-free `ws_command.{hpp,cpp}`, link it into the
host tests, then add `test_ws_command.cpp` BEFORE rewiring `main.cpp` to dispatch via it.
(Attempted earlier and reverted because the test couldn't yet link real code — now
unblocked by the IDF-free header convention.)
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
