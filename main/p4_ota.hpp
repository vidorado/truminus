#pragma once
//
// Self-OTA for the ESP32-P4 application image.
//
// Discovery is done against the public GitHub repo (vidorado/truminus): the
// firmware follows the redirect from `/releases/latest` to read the latest
// tag, then downloads a deterministically-named `truminus.bin` asset via
// esp_https_ota.  No GitHub API JSON parsing and no token are needed.
//
// Trigger model: a background task checks at boot (after WiFi) and every
// CHECK_PERIOD; when a newer release exists it only *flags* it (status +
// LCD + WS).  The actual install is user-initiated (p4OtaInstall()).
//
// Post-OTA safety: a freshly flashed image boots in PENDING_VERIFY.  The
// self-test task (spawned by p4OtaStart) marks it valid only once the
// firmware proves healthy, otherwise the bootloader rolls back.  See the
// .cpp for the gating logic.

#include <stdint.h>
#include <stddef.h>

struct P4OtaStatus {
    bool checking;          // a version check is currently running
    bool available;         // a newer release than the running image exists
    bool installing;        // a download/flash is in progress
    int  progress;          // install progress 0..100 (valid when installing)
    char currentVer[32];    // running firmware version (esp_app_desc_t.version)
    char latestVer[32];     // latest release tag (valid when available)
    char error[64];         // last error message (empty = none)
};

// Spawn the periodic version-check task and — when the running image is in
// PENDING_VERIFY — the post-OTA self-test/rollback task.  Call once from
// bootTask after WiFi has been started.
void p4OtaStart();

// True when the running image has not yet been validated (just OTA-flashed,
// self-test pending).  bootTask uses it to defer heavy DRAM consumers (the WSS
// tunnel) out of the self-test heap-floor window.  Safe to call before
// p4OtaStart().
bool p4OtaPendingVerify();

// Thread-safe snapshot of the current OTA status.
void p4OtaGetStatus(P4OtaStatus& out);

// Kick an immediate version check (non-blocking; result lands in status).
void p4OtaCheckNow();

// Begin downloading + flashing the latest known release.  Non-blocking:
// spawns the install task, which shows progress on the LCD, broadcasts it
// over WS, and reboots on success.  No-op if no update is available or an
// install is already running.
void p4OtaInstall();

// True while a download/flash is in progress.
bool p4OtaInstalling();

// ── Automatic-check preference (NVS "ota"/"autocheck", default on) ────────
// When off: no periodic checks, no topbar reminder icon, no prompt modal.
// Manual checks (p4OtaCheckNow / the settings "Check" button) still work.
bool p4OtaAutoCheckEnabled();
void p4OtaSetAutoCheck(bool enabled);

// ── Topbar reminder + prompt ─────────────────────────────────────────────
// True when a newer release exists AND auto-check is on — drives the topbar
// cloud-download reminder icon.
bool p4OtaNotify();

// One-shot prompt: returns true when there is an available update we have not
// yet prompted the user about (and auto-check is on), filling cur/latest.
// Does NOT mark it shown — call p4OtaMarkPrompted() once the modal is
// actually displayed so a deferred-screen retry still works.
bool p4OtaPromptPending(char* cur, size_t cur_len, char* latest, size_t latest_len);
void p4OtaMarkPrompted();

// ── Liveness heartbeats ──────────────────────────────────────────────────
// Critical tasks call p4OtaBeat() once per iteration so the post-OTA
// self-test can confirm they are still scheduling.  Cheap (one increment);
// safe to call always, not just after an OTA.
enum P4OtaComp : uint8_t {
    P4OTA_BEAT_LOOP = 0,    // app_main display/refresh loop
    P4OTA_BEAT_WEB  = 1,    // wsPumpTask (web/WS)
    P4OTA_BEAT_LIN  = 2,    // LIN scheduler task
    P4OTA_BEAT_COUNT
};
void p4OtaBeat(P4OtaComp comp);
