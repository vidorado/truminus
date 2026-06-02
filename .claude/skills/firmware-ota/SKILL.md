---
name: firmware-ota
description: TruMinus P4 self-OTA — GitHub-Releases-direct update of the application image, versioning via git describe, minor/major auto-prompt, transfer tuning, and the PENDING_VERIFY self-test/rollback net. Read before touching main/p4_ota.cpp, the release workflow, or the OTA settings/web UI.
---

# Firmware self-OTA (`main/p4_ota.cpp`)

Over-the-air updates of the **P4 application image**. The C6 co-processor path
is separate (`c6_ota.cpp`, reflashes the C6 from an embedded binary). Origin:
**GitHub Releases direct**, against `vidorado/truminus`.

## Versioning

`PROJECT_VER` is derived from `git describe --tags --dirty --always` in the root
`CMakeLists.txt` *before* `project()`, so it lands in `esp_app_desc_t.version`.
On a clean tag it is exactly `X.Y.Z`; off-tag it is `X.Y.Z-g<sha>-dirty`. We
strip git describe's "commits since tag" count with
`string(REGEX REPLACE "-[0-9]+-g" "-g" …)` — `1.2.2-2-g3fe4ecf` → `1.2.2-g3fe4ecf`
— purely cosmetic, since `parse_semver()` reads only the leading `X.Y.Z` from
both the running version and the GitHub tag (the suffix is ignored). The `g<sha>`
is the **leading** abbreviated commit hash (GitHub-style short SHA, first 7
chars), not the trailing chars.

`git describe` runs at **configure** time and cmake won't reconfigure on a git
change by itself — so the root `CMakeLists.txt` lists `.git/logs/HEAD`,
`.git/packed-refs` **and `.git/refs/tags`** in `CMAKE_CONFIGURE_DEPENDS`, forcing
a reconfigure after a commit/checkout/tag. It's reconfigure-only; ninja still
builds incrementally. `.git/index` is deliberately **not** watched (it changes
on every `git add`).

**Gotcha — tagging an already-built commit bakes a stale version.** `git tag`
does not move HEAD (no `logs/HEAD` change) and a loose tag does not touch
`packed-refs`. Before `refs/tags` was watched, `git tag X.Y.Z` on the current
HEAD did **not** trigger a reconfigure, so the board kept the previous
`X.Y.(Z-1)-<n>-g<sha>-dirty` baked from the last configure. Editing source files
recompiles via ninja but does **not** reconfigure, so a plain `make build` after
tagging would not fix it either — only a commit/checkout, a `CMakeLists.txt`
edit, or `idf.py reconfigure`/`fullclean`. Watching `refs/tags` (its dir mtime
bumps when a loose tag is created/deleted) closes this; packed tags are still
covered by `packed-refs`. We once flashed `1.1.0-dirty` (and later
`1.2.2-2-g3fe4ecf-dirty` while tag `1.2.3` already existed on HEAD) this way.
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

**Memory-tight download — SDIO RX streaming can assert intermittently.**
`esp_hosted`'s streaming RX mode makes `sdio_rx_get_buffer()` (sdio_drv.c) do a
dynamic, burst-sized DMA-capable `_h_malloc_align()` and then `assert(*buf)` —
so when that internal-DRAM allocation fails under a high-throughput OTA
download the device *panics* (`assert failed: sdio_rx_get_buffer (*buf)`,
sdio_drv.c:953/957) instead of degrading. A fully-provisioned board (tunnel +
BLE) hit this mid-download (begin/get_img_desc had already succeeded) on 1.1.8,
**even though mbedtls is already moved to PSRAM** (`sdkconfig.defaults` lines
~20-41, which previously fixed this same assert during the WSS handshake) and
the install task already suspends the tunnel + Victron + Ultimatron BLE.
Measured: steady free internal DRAM during a real download is **~14 KB**
(`OTA transfer min free internal DRAM = 14055 B`) — and the *same* image
downloaded fine on the next try, so the assert is **intermittent**
(fragmentation-/burst-unlucky), not deterministic. Mitigation (not a switch to
packet mode — that bricks the transport, see below): **adaptive download
backpressure** — when `free_int < OTA_DL_PACE_FLOOR` (20 KB) the perform loop
adds a `OTA_DL_PACE_MS` (5 ms) delay, so the consumer slows, TCP backpressure
shrinks the SDIO bursts, and the streaming alloc stays small. A roomy board
still downloads full speed.

**Do NOT "fix" it by switching the host to packet mode**
(`CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_MAX_SIZE`/`_RX_NONE`). The C6 *slave*
firmware (`main/slave_fw/network_adapter.bin`, built by `make build-c6`) is
compiled in streaming mode, and the transport does a startup handshake that
**aborts** on mismatch: `transport: SDIO mode mismatch: slave is in streaming
mode, but host is in packet mode. Aborting.` → `assert failed:
process_init_event transport_drv.c:879`. Both sides must agree, and a config-
only slave change does **not** bump the slave version, so `c6OtaNeeded()`
(version compare) won't auto-reflash the C6 to match. (Tried + reverted in the
1f52192/83753d1 pair.)

The install loop logs `free_int`/`min` per decile + `OTA transfer min free
internal DRAM = N B` so the margin stays visible. If the adaptive pacing turns
out not to be enough on an even busier board, the remaining levers are: (a)
free more internal DRAM during the window (what else to suspend?); (c) the
clean but heavy route — rebuild the C6 slave in packet mode *and* bump its
version so the C6-OTA migrates it, then set packet mode on the host too (note
the transport-mode bootstrap trap above makes this risky). Any download-side
fix lives in the *running* image, so a stuck board must be **USB-flashed once**
onto the fixed build first.

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
  floor must sit well below normal operation. **`1.2.0` still rolled back on a
  fully-provisioned board** (tunnel enabled + BLE configured) while passing on a
  bare test bench: the boot-time *concurrent* DRAM peak — WSS tunnel TLS
  handshake + Ultimatron GATT connect + WiFi bring-up all at once — sat below
  the floor long enough to trip. The fix wasn't the floor value; it was
  removing the tunnel from the critical window (see *Boot sequencing* below).
  `1.2.1` validated cleanly on the real board with **min=22 KB** — i.e. with
  the tunnel deferred the worst dip sits near the ~24 KB steady-state, ~10 KB
  above the floor. The self-test logs that **minimum** internal-DRAM watermark
  (`min=…`) every ~15 s and on pass (`min free_int during self-test was N B,
  floor 12288 B`) — that number, not a breach, is what tells you the real
  margin.
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

## Boot sequencing (PENDING_VERIFY only)

The self-test floor is measured during the first ~minute, exactly when boot
brings up everything at once. The heaviest internal-DRAM/SRAM consumer is the
**WSS tunnel TLS handshake** (esp-aes DMA contexts must live in SRAM). So on a
PENDING_VERIFY boot we **defer the tunnel**: `bootTask` calls
`wstunnelInit(p4OtaPendingVerify())` — `deferConnect=true` sets up the task +
event handlers but skips the handshake (icon stays CONNECTING). The self-test
calls `wstunnelApply()` right after `esp_ota_mark_app_valid_cancel_rollback()`,
so the handshake then runs **alone**, with WiFi/BLE already settled. Normal
(already-validated) boots pass `deferConnect=false` and are unchanged.

Side effect: with the tunnel deferred, `env_ready()` sees it CONNECTING (not
CONNECTED), so the 30 s fast-pass can't fire — a PENDING_VERIFY boot validates
at the **60 s ceiling**, then the tunnel comes up. Acceptable; the ceiling is
the guarantee. If a board still rolls back with the tunnel deferred, the next
suspect is the **Ultimatron GATT connect** — defer/suspend BLE similarly.

## CI / release builds (one build per release)

`.github/workflows/release.yml` builds on every `X.Y.Z` tag and is the **only**
auto-running build. `ci.yml` is **`workflow_dispatch`-only**: it used to run on
every push to master purely to warm the default-branch ccache, but the
`commit + push master + push tag` flow then fired *two* builds for one commit.
Releases are not forced cold by this: caches are scoped per ref and any ref can
restore the **default-branch** cache, so `release.yml`'s `restore-keys:
ccache-esp32p4-` still picks up the most recent surviving master cache (left by
the last manual `ci` run) and stays ~1–2 min until GitHub evicts it (7 days
idle / LRU past 10 GB). When a release goes cold, run `ci` manually once
(Actions → ci → Run workflow on master) to refresh that cache.

## UI surfaces

Install progress on the LCD OTA screen + WS `{"command":"ota",…,"installing":true,"progress":N}`.

The LCD **"Actualizaciones" settings screen** (`show_updates` in
`main/p4settings.cpp`) **pauses BLE on entry and resumes it in `upd_back_cb`**
(`victronBleSuspend()`/`ultimatronBleSuspend()` — flag-only, idempotent). BLE's
continuous scan window (RX-range recovery under C6 coex) otherwise starves WiFi
enough that the GitHub version check hangs on "Checking…" and a download
crawls. The install path manages BLE itself (suspend + resume-on-fail / reboot)
and never returns to this screen, so it can't be left paused.
