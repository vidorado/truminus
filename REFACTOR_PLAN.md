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

**No inline copies remain.** Every host test links the real production `.cpp`;
`test_helpers.{cpp,hpp}` has been deleted.

## Current state

- **Host test suite:** 476 assertions across 68 `TEST_CASE`s (Catch2 v3.5.2),
  built via `test/host/build.sh`, run with `ctest` or `./build/tests`.
- **IDF-free modules linked into the host tests (real production code, not copies):**
  `mode_controller` (fan/boiler + `derive_mode`), `ws_command` (WS frame parsing),
  `ws_diff` (broadcast change-detection), `version_compare` (semver),
  `lin_codec` (Kelvin temp encode/parse), `lin_protocol` (PID/checksum),
  `lin_frames` (0x20/0x05/0x06/0x07 field encoders), `bthome_codec`, `am2301_codec`,
  `multiplus_codec` (BitReader + VE.Bus unpack), `ultimatron_codec`, `victron_codec`,
  `faultlog_codec`. Plain data structs they share live in IDF-free headers
  (`control_state.hpp`; the BLE `*Data` structs were already IDF-free).
- Each extraction verified: host suite green **and** firmware build green. The LIN
  0x20 control path was additionally validated end-to-end on the bench against the
  hw simulator's decoded master-command log.

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

### LIN low-level + frame encoders ✅ DONE
`lin_protocol` (PID/checksum, `LinDriver` methods now delegate) and `lin_frames`
(0x20/0x05/0x06/0x07 field encoders, `f20_set*`/… now bind them to the frame buffers)
extracted; tests repointed; `test_helpers` deleted.

### 1. WebSocket command parsing ✅ DONE
Extracted to `ws_command.{hpp,cpp}` with `test_ws_command.cpp` linking the real parser;
`onWsCommand()` is now just a dispatch switch. Tests written and green BEFORE rewiring
`main.cpp` (the order that was missed on the first, reverted attempt).

### 2. Broadcast diff logic ✅ DONE (scoped)
The change-detection predicates (the part with real edge cases) were extracted into
`ws_diff.{hpp,cpp}` and tested: `victronChanged`, `ultimatronChanged`, `multiplusChanged`
(NaN-battV aware), `tankChanged` (fresh-advert `lastMs` re-emit), `multiPowerChanged`
(MULTI_POWER_NA sentinel, no abs overflow), `linTempChanged`. The JSON formatting was
**deliberately left inline** in `main.cpp` (low ROI: it reads globals into a buffer and
enqueues). No heavyweight `WsBroadcaster` class — small free functions, as planned.

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
