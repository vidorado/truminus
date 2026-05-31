---
name: firmware-ota
description: TruMinus P4 self-OTA — GitHub-Releases-direct update of the application image, versioning via git describe, minor/major auto-prompt, transfer tuning, and the PENDING_VERIFY self-test/rollback net. Read before touching main/p4_ota.cpp, the release workflow, or the OTA settings/web UI.
---

# Firmware self-OTA (`main/p4_ota.cpp`)

Over-the-air updates of the **P4 application image**. The C6 co-processor path
is separate (`c6_ota.cpp`, reflashes the C6 from an embedded binary). Origin:
**GitHub Releases direct**, against `vidorado/truminus`.

## Versioning

`PROJECT_VER` is derived from `git describe --tags` in the root `CMakeLists.txt`
*before* `project()`, so it lands in `esp_app_desc_t.version`. On a clean tag it
is exactly `X.Y.Z`; in a dirty tree it is `X.Y.Z-<n>-g<sha>-dirty`.
`parse_semver()` reads the leading `X.Y.Z` from both the running version and the
GitHub tag. `git describe` runs at **configure** time and cmake won't
reconfigure on a git change by itself — so the root `CMakeLists.txt` lists
`.git/logs/HEAD` + `.git/packed-refs` in `CMAKE_CONFIGURE_DEPENDS`, forcing a
reconfigure after a commit/checkout (we once flashed `1.1.0-dirty` from a stale
cached `PROJECT_VER`). It's reconfigure-only; ninja still builds incrementally.
`.git/index` is deliberately **not** watched (it changes on every `git add`).
See also pio-idf-p4 SKILL §10.

## Discovery without the API

`GET /releases/latest` with `disable_auto_redirect = true`; GitHub answers
`302 → /releases/tag/<tag>` and the tag is the last path segment of the
`Location` header. Tiny, no JSON, no token, immune to the 60 req/h
unauthenticated API limit.

## Asset name is a contract

The release **must** carry an asset named exactly `truminus.bin`; the firmware
builds `…/releases/download/<tag>/truminus.bin` and lets `esp_https_ota` follow
the 302 to the release CDN (covered by the IDF cert bundle).
`.github/workflows/release.yml` builds on every `X.Y.Z` tag and uploads that
asset (the C6 binary is absent in CI — the app builds fine without it).

## Trigger model

Auto-check at boot + every 12 h; an update is only *flagged* (LCD
"Actualizaciones" settings screen + web banner). Install is **user-initiated**
(`p4OtaInstall()` → LCD "Update" button or web).

**Auto-prompt only for minor/major.** The periodic check sets
`s_status.available` for **any** newer tag (so the LCD screen, the `ota` CLI and
the web banner surface patches), but the *proactive* topbar reminder icon
(`p4OtaNotify()`) and the prompt modal (`p4OtaPromptPending()`) fire only when
the latest is a **minor or major** bump (`is_minor_or_major_newer()` →
`s_notify_worthy`). Patch releases don't nag; the user still finds + installs
them via a manual check.

## Transfer tuning (download speed)

During the transfer `install_task` frees the radio + DRAM: `wstunnelSuspend()`
(DRAM for the TLS handshake) **and** `victronBleSuspend()` +
`ultimatronBleSuspend()` — the C6 shares one radio between WiFi and BLE, so an
active BLE scan throttles the download. The HTTP client RX buffer is **16 KB**
(matches `MBEDTLS_SSL_IN_CONTENT_LEN`, so a full TLS record is read per call;
the default 1 KB also broke `esp_https_ota_begin` against the long signed CDN
redirect URL → "HTTP_CLIENT: Out of buffer"). A throughput log
(`OTA download: N bytes in T s = X KB/s avg` + per-decile) tells whether the
bottleneck is the network or the serialized flash writes. The dominant
real-world speed fix was **WiFi power save** (pio-idf-p4 SKILL §12).

## Rollback safety net

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (+ the pre-existing
`CONFIG_BOOTLOADER_WDT_ENABLE`). A freshly flashed image boots `PENDING_VERIFY`;
`selftest_task` (spawned only in that state) marks it valid via
`esp_ota_mark_app_valid_cancel_rollback()` **only after** it proves healthy.
Gating splits **hard firmware-health signals** from **best-effort environment
signals** (IP, tunnel CONNECTED, fresh LIN frame, BLE advert):

- *Heap floor* `HEAP_FLOOR=12 KB` of internal DRAM, **sustained** — only rolls
  back after `HEAP_BREACH_LIMIT=5` consecutive 2 s samples below it, so a
  transient handshake dip doesn't false-trip. Steady-state free internal DRAM
  here is ~24 KB, so an earlier 24 KB floor rolled back a *healthy* image; the
  floor must sit well below normal operation.
- *Heartbeats* via `p4OtaBeat()` from the main loop / wsPump / LIN task; a task
  not beating for `BEAT_STALL_MS=20 s` rolls back. (Critical tasks must call
  `p4OtaBeat()` so the self-test can confirm liveness.)
- Hard-gate failure → proactive
  `esp_ota_mark_app_invalid_rollback_and_reboot()`, and the reason + free-heap
  value are **persisted to NVS** (`ota/rb_why`, `rb_heap`) and logged loudly on
  the next boot (`report_prior_rollback()`) — the USB-Serial-JTAG console
  re-enumerates on the reboot, so the pre-rollback `ESP_LOGE` is otherwise lost.
- Timing: **fast-pass at 30 s** if the environment is ready, else a **60 s
  ceiling** validates regardless (was 90 s / 8 min — far too long a silent
  `PENDING_VERIFY` window when the env never comes up, e.g. Combi off → LIN
  never ready). A ~15 s progress log shows elapsed / free heap / `env_ready`.
  The net is one-shot for the first ~minute; it does **not** guard a crash after
  validation.

## UI surfaces

Install progress on the LCD OTA screen + WS `{"command":"ota",…,"installing":true,"progress":N}`.
