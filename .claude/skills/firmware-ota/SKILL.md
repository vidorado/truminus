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

**Gotcha — "latest" is the most recently *published* release, not the highest
semver.** GitHub picks `/releases/latest` by the release's `published_at`
timestamp (equivalently the last `make_latest:true`), *not* by version number or
commit date. `action-gh-release@v2` defaults to `make_latest: true`, so **the
release whose workflow finishes last wins**. Pushing several tags at once (e.g.
`1.2.4` + `1.2.5` together) runs the build workflows concurrently; if the older
tag's run happens to finish last, GitHub serves *it* as latest and the firmware
never sees the newer one. Operational rule: **release one tag at a time, and if
you push several, let the highest version's run finish last.** To repair after
the fact, force it: `gh api -X PATCH repos/<o>/<r>/releases/<id> -f make_latest=true`
on the higher version (the `api.github.com` `/releases/latest` updates
immediately; the `github.com` web redirect the firmware follows is CDN-cached
and lags a few minutes). We hit this with `1.2.4` finishing ~10 s after `1.2.5`.
A semver-correct alternative would enumerate `/releases` and pick the max
version, but that needs the rate-limited JSON API this design deliberately avoids.

**Cert gotcha — the real cause was the P4 hardware ECC accelerator, NOT the
cert chain.** The version check (`fetch_latest_tag` →
`https://github.com/.../releases/latest`) failed with `esp-x509-crt-bundle:
Failed to verify certificate` / `mbedtls_ssl_handshake returned -0x3000`
(that code is `MBEDTLS_ERR_X509_FATAL_ERROR`, the bundle callback aborting; the
underlying x509 result is `-0x2700` `CERT_VERIFY_FAILED`). It coincided with
**GitHub migrating DigiCert → Sectigo**, so the obvious-but-wrong theory was the
new Sectigo/USERTrust **ECDSA** chain. Two failed releases chased that: 1.2.25
added `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y` (no effect);
1.2.26-rc pinned the roots via `cfg.cert_pem` (no effect — still `-0x2700`).

The decisive test was **disabling cert verification entirely**
(`CONFIG_ESP_TLS_INSECURE=y` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`, no CA
on the client): the handshake *still* failed, now at `-0x0095`
(`PSA_ERROR_INVALID_SIGNATURE`) on the TLS 1.2 ServerKeyExchange — proving the
problem was **never the certificate**. Both failures are the same root cause:
**every ECDSA signature verification returns INVALID_SIGNATURE**, because the
ESP32-P4 hardware ECC accelerator produces wrong results for ECDSA verify on
this board. So github.com / codeload (ECDSA leaf + Sectigo/USERTrust ECC chain)
could never be verified, while RSA peers — the Let's Encrypt release CDN
(`*.githubusercontent.com`, ISRG Root X1) and the tunnel bridge — worked fine,
which is exactly why downloads succeeded but the metadata check didn't.

**The fix (1.2.26): `CONFIG_MBEDTLS_HARDWARE_ECC=n`** (software ECC) in
`sdkconfig.defaults` — MUST be in defaults, the IDF default is `=y` and a
fullclean would bring the bug back. `HARDWARE_ECDSA_VERIFY` was already off but
`HARDWARE_ECC` (the ECP point-mul accel) still routed verify through the buggy
peripheral. With software ECC the plain `esp_crt_bundle` verifies github.com's
chain fine, so the cert-pinning / cross-signed scaffolding was all reverted.
Software ECC is slower but TLS handshakes here are rare (OTA + one tunnel
connect). TLS 1.3 is also disabled in this build (`MBEDTLS_SSL_PROTO_TLS1_3`
unset) so everything negotiates TLS 1.2 — not the cause, but worth knowing.

Debugging lesson: trust the *error code*, not the *symptom*. `-0x3000` is
FATAL not "verify failed"; the esp-tls `flags=` channel read 0 even on real
failures (needs `KEEP_PEER_CERTIFICATE`, and even then was unreliable). What
cracked it was the no-verify isolation test + `CONFIG_MBEDTLS_DEBUG=y` level 4
showing the handshake reach the cert step and the PSA signature error. Not a
clock/SNTP issue — `MBEDTLS_HAVE_TIME_DATE` is off and there is no SNTP.

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

**Forcing a patch to prompt (important / security fix).** SemVer has no
"importance" field, so the signal lives out-of-band in the release: attach a
zero-byte asset named **`force-notify`** to the GitHub release. When an update is
available, `release_has_force_notify()` does one header-only request to that
asset's download URL (302 = present, 404 = absent — the same redirect trick as
`fetch_latest_tag`, no GitHub API/auth), and `s_notify_worthy` becomes
`newer && (minor/major || force-notify present)`. So a patch that ships the
marker prompts like a minor would. To cut such a release:
`gh release upload <tag> /dev/null --clobber` won't name it; create the marker
explicitly, e.g. `: > force-notify && gh release upload <tag> force-notify`.
Note: only firmware **already running** this code reads the marker, so it takes
effect from the *next* release onward.

## Transfer tuning (download speed)

During the transfer `install_prep_task` fully tears down NimBLE
(`bleSupervisorStop()` → frees tens of KB of host RAM) before spawning the
download — the C6 shares one radio between WiFi and BLE, so an active BLE scan
throttles the download. **The WSS tunnel is deliberately LEFT UP**, even though
its httpd per-socket scratch + websocket-client buffers sit in internal DRAM: a
remote user watches the install **progress bar over that very tunnel**, so
suspending it blinds them for the whole download. Freeing that DRAM was tried
(`wstunnelSuspend()` in 1.3.6) and **reverted in 1.3.8** — the progress-bar loss
isn't worth it, and the SDIO assert it dodged is intermittent (clears on a
retry). Do not re-add it. `wstunnelSuspend()` remains defined but unused. The HTTP client RX buffer is **16 KB**
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
the install task already tears down NimBLE (Victron + Ultimatron) — but the
tunnel is deliberately kept up (see Transfer tuning), so its DRAM is still in play.
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

**`gh` resolves to the wrong repo by default.** A `gh` command with no `--repo`
picks the upstream (`olivluca/TruMinus`, the fork parent) and 404s on our
Actions/releases. **Always pass `--repo vidorado/truminus`** — e.g.
`gh run list --repo vidorado/truminus --workflow=release.yml`,
`gh run watch <id> --repo vidorado/truminus`, `gh release ... --repo vidorado/truminus`.

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

## Web-asset (LittleFS) sync

The app OTA (`truminus.bin`) updates only the **application** partition; the web
UI lives on the separate `littlefs` data partition. After an app image
validates, `littlefs_sync()` brings the web in line with the release:

- **Marker, not blob, drives the decision.** `gen_fs_ver.py` (run by
  `web_assets_prep`, a prerequisite of `littlefs_littlefs_bin`) hashes all of
  `data/` into `data/fs.ver`, baked into the image as `/littlefs/fs.ver`. CI
  copies that same file to the release asset `littlefs.ver`. The device fetches
  the tiny marker and compares; equal → skip the ~500 KB download entirely.
  (How the LittleFS image itself is built/flashed — `littlefs_create_partition_image`,
  the rebuild-every-cmake-run quirk, `--skip-flashed`, the VSCode Flash-button
  trap — is **pio-idf-p4 §13**.)
- **Assets** (release contract, all three or `fail_on_unmatched_files` trips):
  `truminus.bin`, `littlefs.bin.gz` (the 8 MB image is ~95% 0xFF → gzips to a
  few hundred KB; inflated on-flash by `inflate_to_partition` via ROM `tinfl`),
  `littlefs.ver`. Feature first shipped in **1.2.18**.
- **Triggers, in order:** (1) the self-test calls `littlefs_sync_async()` right
  after marking the image valid; (2) `p4OtaStart()` reconciles on **every**
  normal boot — *unconditionally*, targeting the running tag (a cheap no-op when
  `/littlefs/fs.ver` already matches); (3) a frequent **LOCAL** reconcile
  (`fs_reconcile_local`, ~30 s from `check_task`) compares `/littlefs/fs.ver`
  against the marker GitHub last reported (cached in `s_want_fs_ver` on every
  fetch) and kicks a full sync **only on a mismatch**, rate-limited to ≥3 min so
  a failing download can't hammer the shared radio. `fs_sync_task` waits up to
  60 s for an IP before fetching.

  > **Do NOT gate the boot retry on `fs_pending` alone (the old design, fixed
  > 1.3.9).** A sync that failed in a way that cleared or never set that flag —
  > notably a spurious 404 → `ABSENT` → `fs_pending_clear()` — left the web
  > **permanently** stale despite the mismatch, boot after boot (field: app
  > 1.3.7, web stuck at 1.3.5). The unconditional boot reconcile + the frequent
  > local check are the self-healing net: they act on the real web≠app mismatch,
  > which no lost flag can hide. `fs_pending` still exists but is now just a hint,
  > not the sole trigger.

**`fetch_fs_ver` MUST distinguish 404 from a transport error** (`FsVerResult`
tri-state). They demand opposite handling and conflating them was a real field
bug:

| Outcome | Meaning | Action |
|---------|---------|--------|
| `OK` | got the marker | compare; sync if it differs |
| `ABSENT` (404) | release predates the feature | `fs_pending_clear()` — nothing to do, ever |
| `ERROR` (-1 / other) | DNS/TLS/CDN hiccup | `fs_pending_set(tag)` — boot retry picks it up |

**The bug this fixed:** the old code returned `bool` and treated any failure as
"skip" with **no `fs_pending`**, so a transient fetch failure during the
PENDING_VERIFY boot (network still settling — exactly when the self-test runs
the sync) lost the sync until the *next* OTA. Seen in the field: a board on
**1.2.15** (pre-feature, no `/littlefs/fs.ver`) OTA'd to 1.2.23 and kept serving
the old web because the boot-time `fetch_fs_ver` blipped and never retried.
`fs_pending` is now set on `ERROR`, so the next boot resyncs.

**`fetch_fs_ver` MUST pause BLE like the version check does — this was the
deciding bug.** `littlefs_sync()` originally suspended BLE only around the
`.gz` *download*; the marker `fetch_fs_ver()` ran with BLE still scanning. On
the shared C6 radio a live BLE scan starves WiFi, and the 65-byte marker fetch
loses the race **whenever the WiFi signal margin is thin** — so a
fully-provisioned board *in the field* (weak signal) never synced, while a
provisioned bench with strong coverage did. The tell: on the *same* field
board the periodic version check reached GitHub fine (it suspends BLE +
waits the scan window, lines ~335-344) but the fs.ver fetch didn't — same
board, same BLE, the only difference was the suspend. Fix: `littlefs_sync()`
now suspends BLE + waits out the in-flight scan window for the **whole**
network section (fetch + download), mirroring `check_and_notify()`, and resumes
on every return path. Don't reorder the suspend back below the fetch.
(Signal margin, not BLE present/absent, is the discriminator — both bench and
field boards run BLE.)

Diagnosing from the device: the LCD **Actualizaciones** screen / web About
overlay shows the first 12 hex of `/littlefs/fs.ver` (`p4OtaFsVersion`); "—"
means no marker (pre-feature web still flashed). Serial log tells which branch
ran: `web up to date` / `no littlefs.ver … pre-feature release` / `fetch …
failed — will retry next boot` / `web differs … updating`.

## UI surfaces

Install progress on the LCD OTA screen + WS `{"command":"ota",…,"installing":true,"progress":N}`.

The LCD **"Actualizaciones" settings screen** (`show_updates` in
`main/p4settings.cpp`) **pauses BLE on entry and resumes it in `upd_back_cb`**
(`victronBleSuspend()`/`ultimatronBleSuspend()` — flag-only, idempotent). BLE's
continuous scan window (RX-range recovery under C6 coex) otherwise starves WiFi
enough that the GitHub version check hangs on "Checking…" and a download
crawls. The install path manages BLE itself (suspend + resume-on-fail / reboot)
and never returns to this screen, so it can't be left paused.
