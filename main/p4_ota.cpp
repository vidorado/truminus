#include "p4_ota.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   // xTaskCreateWithCaps / vTaskDeleteWithCaps
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"

#include "p4display.hpp"
#include "i18n.hpp"
#include "webserver.hpp"
#include "wstunnel.hpp"
#include "wifi_manager.hpp"
#include "truma_lin.hpp"
#include "victronble.hpp"
#include "ultimatronble.hpp"
#include "tankble.hpp"
#include "multiplusble.hpp"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <atomic>

static const char* TAG = "p4_ota";

// ── Configuration ─────────────────────────────────────────────────────────
// GitHub repo to track and the deterministic asset name the release must
// carry.  The CI workflow (.github/workflows/release.yml) uploads the built
// firmware under exactly this name.
#define OTA_GH_OWNER  "vidorado"
#define OTA_GH_REPO   "truminus"
#define OTA_ASSET     "truminus.bin"
#define OTA_LATEST_URL "https://github.com/" OTA_GH_OWNER "/" OTA_GH_REPO "/releases/latest"

// Period between automatic version checks (after the boot check).
static constexpr uint32_t CHECK_PERIOD_MS = 12 * 60 * 60 * 1000;  // 12 h

// Delay before the *first* (boot) check.  The C6 shares one radio between WiFi
// and BLE, so the HTTPS version check starves the BLE scan that brings up the
// Victron/Ultimatron monitors.  Hold the boot check off until the BLE
// supervisor (starts ~15 s in) has had a clean window to read present devices
// and time out absent ones.
static constexpr uint32_t INITIAL_CHECK_DELAY_MS = 60 * 1000;  // 60 s

// ── Post-OTA self-test gating ───────────────────────────────────────────
// Hard firmware-health gates (independent of the environment):
// Steady-state free internal DRAM on this board sits around ~24 KB (WiFi via
// the C6, the WSS tunnel's 16 KB TLS buffers, LVGL, BLE).  The floor must sit
// well below that so it only fires on a real leak/exhaustion (which drives the
// heap toward zero), not on healthy operation — a 24 KB floor rolled back a
// perfectly healthy OTA'd 1.1.4 at free_int=24455.
static constexpr size_t   HEAP_FLOOR        = 12 * 1024;  // internal DRAM
static constexpr int      HEAP_BREACH_LIMIT = 5;          // consecutive samples below floor → rollback
static constexpr uint32_t BEAT_STALL_MS     = 20000;      // task considered dead
// Validation timing.  Kept just above the hard gates (heap 10 s, task-stall
// 20 s) so a real failure still has time to trip before we validate — but far
// below the old 90 s/8 min, which left the image PENDING_VERIFY (and the LCD
// silent) for ages whenever the environment never came up (e.g. Combi off →
// LIN never ready, the common case).
static constexpr uint32_t SELFTEST_FAST_MIN_MS = 30 * 1000;   // earliest validate (env ready)
static constexpr uint32_t SELFTEST_CEILING_MS  = 60 * 1000;   // validate regardless
static constexpr uint32_t SELFTEST_SAMPLE_MS   = 2000;

// Download backpressure.  esp_hosted's streaming SDIO RX does a burst-sized
// DMA-capable alloc per transaction and HARD-ASSERTS on a NULL (sdio_drv.c).
// Measured steady free internal DRAM during a real download is ~14 KB — thin
// enough that a fragmentation-unlucky burst can fail intermittently (it did,
// once).  When headroom drops below the floor we add a small delay in the
// perform loop: the consumer slows, TCP backpressure shrinks the incoming
// bursts, and the streaming alloc stays small.  Adaptive, so a board with
// plenty of headroom still downloads at full speed.
static constexpr size_t   OTA_DL_PACE_FLOOR = 20 * 1024;   // free-int below this → pace
static constexpr uint32_t OTA_DL_PACE_MS    = 5;

// ── Shared status ───────────────────────────────────────────────────────
// Created at static-init time (C++ global constructors run on the main task
// with the FreeRTOS scheduler already up) so the public getters are safe to
// call from the display loop before bootTask reaches p4OtaStart().
static SemaphoreHandle_t s_lock = xSemaphoreCreateMutex();
static P4OtaStatus       s_status = {};
static std::atomic<bool> s_installing{false};
static char              s_asset_url[256] = "";

// Heartbeat counters bumped by the critical tasks.
static std::atomic<uint32_t> s_beats[P4OTA_BEAT_COUNT];

// Set by p4OtaCheckNow() to interrupt the check task's sleep.
static std::atomic<bool> s_check_request{false};

// Automatic-check preference + prompt bookkeeping (guarded by s_lock).
static bool s_autocheck      = true;
static char s_prompted_ver[32] = "";   // version we've already prompted about
// True when the latest release is a minor/major bump over the running image
// (set by do_check).  Gates the proactive topbar icon + prompt modal so a
// patch-only release doesn't nag; s_status.available stays true for any newer
// version so manual checks / install still surface patches.
static bool s_notify_worthy  = false;

void p4OtaBeat(P4OtaComp comp) {
    if (comp < P4OTA_BEAT_COUNT) s_beats[comp].fetch_add(1, std::memory_order_relaxed);
}

static void status_set_error(const char* msg) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_status.error, sizeof(s_status.error), "%s", msg ? msg : "");
    xSemaphoreGive(s_lock);
}

void p4OtaGetStatus(P4OtaStatus& out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out = s_status;
    xSemaphoreGive(s_lock);
}

bool p4OtaInstalling() { return s_installing.load(); }

bool p4OtaAutoCheckEnabled() {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_autocheck;
    xSemaphoreGive(s_lock);
    return v;
}

void p4OtaSetAutoCheck(bool enabled) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_autocheck = enabled;
    xSemaphoreGive(s_lock);
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "autocheck", enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "auto-check %s", enabled ? "enabled" : "disabled");
}

bool p4OtaNotify() {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Proactive topbar reminder: only for minor/major bumps (patch is silent).
    bool v = s_status.available && s_autocheck && s_notify_worthy;
    xSemaphoreGive(s_lock);
    return v;
}

bool p4OtaPromptPending(char* cur, size_t cur_len, char* latest, size_t latest_len) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Proactive modal: only for minor/major bumps (patch never auto-prompts).
    bool pend = s_status.available && s_autocheck && s_notify_worthy &&
                strcmp(s_prompted_ver, s_status.latestVer) != 0;
    if (pend) {
        snprintf(cur,    cur_len,    "%s", s_status.currentVer);
        snprintf(latest, latest_len, "%s", s_status.latestVer);
    }
    xSemaphoreGive(s_lock);
    return pend;
}

void p4OtaMarkPrompted() {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_prompted_ver, sizeof(s_prompted_ver), "%s", s_status.latestVer);
    xSemaphoreGive(s_lock);
}

// Broadcast the current OTA status to web clients.  Shape consumed by
// data/script.js: {"command":"ota","available":bool,"installing":bool,
// "progress":N,"current":"x","latest":"y","error":"…"}.
static void broadcast_status() {
    P4OtaStatus st;
    p4OtaGetStatus(st);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"command\":\"ota\",\"available\":%s,\"installing\":%s,"
             "\"progress\":%d,\"current\":\"%s\",\"latest\":\"%s\",\"error\":\"%s\"}",
             st.available ? "true" : "false",
             st.installing ? "true" : "false",
             st.progress, st.currentVer, st.latestVer, st.error);
    wsQueueSend(buf);
}

// ── Version helpers ──────────────────────────────────────────────────────
// Parse a leading [v]MAJOR.MINOR[.PATCH] from `s`.  Returns false if no
// numeric major could be read.  Trailing git-describe suffix ("-3-gabc1")
// is ignored.
static bool parse_semver(const char* s, int* mj, int* mn, int* pt) {
    if (!s) return false;
    while (*s == 'v' || *s == 'V' || *s == ' ') s++;
    int a = 0, b = 0, c = 0;
    int n = sscanf(s, "%d.%d.%d", &a, &b, &c);
    if (n < 1) return false;
    *mj = a; *mn = b; *pt = c;
    return true;
}

// >0 if `a` newer than `b`, 0 if equal, <0 if older.  Unparseable → treated
// conservatively as "not newer" (returns <=0).
static int semver_cmp(const char* a, const char* b) {
    int am, an, ap, bm, bn, bp;
    if (!parse_semver(a, &am, &an, &ap)) return -1;
    if (!parse_semver(b, &bm, &bn, &bp)) return 1;
    if (am != bm) return am - bm;
    if (an != bn) return an - bn;
    return ap - bp;
}

// True if `latest` is a newer MINOR or MAJOR than `running` (a patch-only
// bump returns false).  Gates the *proactive* auto-notification (topbar
// reminder icon + prompt modal) so patch releases don't nag the user; manual
// checks (settings screen / `ota check`) and install still use plain
// semver_cmp, so patches are surfaced and installable when explicitly sought.
static bool is_minor_or_major_newer(const char* latest, const char* running) {
    int lm, ln, lp, rm, rn, rp;
    if (!parse_semver(latest, &lm, &ln, &lp)) return false;
    if (!parse_semver(running, &rm, &rn, &rp)) return false;
    if (lm != rm) return lm > rm;   // major bump
    return ln > rn;                 // same major: only a minor increase counts
}

static const char* running_version() {
    const esp_app_desc_t* d = esp_app_get_description();
    return d ? d->version : "0.0.0";
}

// ── Discovery: latest release tag via the /releases/latest redirect ──────
// GitHub answers GET /releases/latest with a 302 → /releases/tag/<tag>.  We
// disable auto-redirect and grab the Location header, then take the last path
// segment as the tag.  Tiny transfer, no JSON, no token.
//
// NOTE: esp_http_client_get_header() returns *request* headers we set, not
// response headers.  The Location *response* header is only delivered through
// the HTTP_EVENT_ON_HEADER callback, so we capture it there.
struct LocCtx { char* buf; size_t len; bool got; };

static esp_err_t loc_evt_handler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key &&
        strcasecmp(evt->header_key, "Location") == 0) {
        LocCtx* ctx = (LocCtx*)evt->user_data;
        if (ctx && evt->header_value) {
            snprintf(ctx->buf, ctx->len, "%s", evt->header_value);
            ctx->got = true;
        }
    }
    return ESP_OK;
}

static bool fetch_latest_tag(char* tag, size_t tag_len) {
    char loc[256] = "";
    LocCtx ctx = { loc, sizeof(loc), false };

    esp_http_client_config_t cfg = {};
    cfg.url                   = OTA_LATEST_URL;
    cfg.crt_bundle_attach     = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = true;
    cfg.timeout_ms            = 10000;
    cfg.event_handler         = loc_evt_handler;
    cfg.user_data             = &ctx;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_http_client_set_header(c, "User-Agent", "TruMinus-OTA");
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");

    bool ok = false;
    esp_err_t e = esp_http_client_open(c, 0);
    if (e == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if ((status == 301 || status == 302 || status == 307 || status == 308) &&
            ctx.got && loc[0]) {
            const char* slash = strrchr(loc, '/');
            if (slash && slash[1]) {
                snprintf(tag, tag_len, "%s", slash + 1);
                ok = true;
            }
        } else {
            ESP_LOGW(TAG, "unexpected status %d (Location=%s)",
                     status, ctx.got ? loc : "<none>");
        }
    } else {
        ESP_LOGW(TAG, "open %s failed: %s", OTA_LATEST_URL, esp_err_to_name(e));
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

// Run one version check; updates s_status and broadcasts.  Blocking
// (network); always called from the check task, never the caller.
static void do_check() {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.checking = true;
    snprintf(s_status.currentVer, sizeof(s_status.currentVer), "%s", running_version());
    xSemaphoreGive(s_lock);
    broadcast_status();

    // Pause BLE for the duration of the network check so WiFi gets the shared
    // C6 radio to itself (the physical Updates screen does the same on entry).
    // Only resume what we paused, so a concurrent screen/install that already
    // suspended it isn't un-paused out from under us.
    bool vicWas = victronBleSuspended();
    bool ultWas = ultimatronBleSuspended();
    if (!vicWas) victronBleSuspend();
    if (!ultWas) ultimatronBleSuspend();

    char tag[32] = "";
    bool got = fetch_latest_tag(tag, sizeof(tag));

    if (!vicWas) victronBleResume();
    if (!ultWas) ultimatronBleResume();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.checking = false;
    if (!got) {
        snprintf(s_status.error, sizeof(s_status.error), "check failed");
    } else {
        s_status.error[0] = '\0';
        bool newer = semver_cmp(tag, s_status.currentVer) > 0;
        s_status.available = newer;
        s_notify_worthy = newer && is_minor_or_major_newer(tag, s_status.currentVer);
        if (newer) {
            snprintf(s_status.latestVer, sizeof(s_status.latestVer), "%s", tag);
            snprintf(s_asset_url, sizeof(s_asset_url),
                     "https://github.com/" OTA_GH_OWNER "/" OTA_GH_REPO
                     "/releases/download/%s/" OTA_ASSET, tag);
            ESP_LOGI(TAG, "update available: %s -> %s (%s)", s_status.currentVer, tag,
                     s_notify_worthy ? "minor/major — will prompt"
                                     : "patch — silent (manual check only)");
        } else {
            // No longer behind — clear the prompt memory so a future release
            // pops the modal again.
            s_prompted_ver[0] = '\0';
            ESP_LOGI(TAG, "up to date (running %s, latest %s)", s_status.currentVer, tag);
        }
    }
    xSemaphoreGive(s_lock);
    broadcast_status();
}

// ── Install ──────────────────────────────────────────────────────────────
static void install_task(void*) {
    char url[256], cur[32], latest[32];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(url,    sizeof(url),    "%s", s_asset_url);
    snprintf(cur,    sizeof(cur),    "%s", s_status.currentVer);
    snprintf(latest, sizeof(latest), "%s", s_status.latestVer);
    s_status.installing = true;
    s_status.progress   = 0;
    s_status.error[0]   = '\0';
    xSemaphoreGive(s_lock);
    broadcast_status();

    ESP_LOGI(TAG, "starting self-OTA from %s", url);
    p4DisplayShowOtaScreen(cur, latest);

    // Free up the radio + internal DRAM for the duration:
    //  - Tear the WSS tunnel down (frees DRAM for the TLS handshake).
    //  - Pause BLE so the C6 stops sharing airtime between WiFi and BLE —
    //    coexistence on the single co-processor otherwise throttles the
    //    download badly.  victronBleSuspend() also stops the tank/multiplus
    //    scan (they piggyback on the same GAP scan).
    // The reboot on success re-establishes everything; on failure we restore
    // it below.
    wstunnelSuspend();
    victronBleSuspend();
    ultimatronBleSuspend();
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_http_client_config_t http = {};
    http.url               = url;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms        = 15000;
    http.keep_alive_enable = true;
    // browser_download_url 302-redirects to the release CDN — follow it.
    http.max_redirection_count = 5;
    // The GitHub 302 points at a long AES-signed objects.githubusercontent.com
    // URL (X-Amz-… query) plus large response headers.  The default 1024 B
    // TX/RX buffers overflow on the redirected request → "HTTP_CLIENT: Out of
    // buffer" and esp_https_ota_begin() fails.  RX matches the 16 KB TLS
    // record (MBEDTLS_SSL_IN_CONTENT_LEN) so each record is read in one go.
    http.buffer_size       = 16384;
    http.buffer_size_tx    = 4096;

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &http;

    esp_https_ota_handle_t h = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &h);
    if (err != ESP_OK || !h) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        status_set_error("download failed");
        goto fail;
    }

    {
        esp_app_desc_t newd;
        if (esp_https_ota_get_img_desc(h, &newd) == ESP_OK) {
            ESP_LOGI(TAG, "remote image version: %s", newd.version);
            // Guard against re-flashing the identical build.
            if (strncmp(newd.version, cur, sizeof(newd.version)) == 0) {
                ESP_LOGW(TAG, "remote image identical to running — aborting");
                status_set_error("already current");
                esp_https_ota_abort(h);
                goto fail;
            }
        }

        int total = esp_https_ota_get_image_size(h);
        int last_pct = -1;
        // Throughput instrumentation: overall average + a sample each decile,
        // so we can tell whether the OTA is gated by the network or by the
        // serialized flash writes (see the C6/SDIO discussion).
        int64_t t_start    = esp_timer_get_time();
        int64_t t_seg      = t_start;   // start of the current decile segment
        int     seg_bytes0 = 0;         // bytes read at the segment start
        // Track the lowest free internal DRAM seen during the transfer.  The
        // esp_hosted SDIO RX path (streaming mode) does a dynamic DMA-capable
        // alloc per burst in sdio_rx_get_buffer() and HARD-ASSERTS if it
        // returns NULL — a fully-provisioned board hit exactly this mid-
        // download even with mbedtls already moved to PSRAM.  Logging the
        // watermark every decile shows how close to zero we run, so the real
        // fix (free more internal RAM / pace the transfer / packet mode on
        // both C6 + host) can be chosen from data rather than guesswork.
        size_t dl_min_int = SIZE_MAX;
        while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            size_t fi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            if (fi < dl_min_int) dl_min_int = fi;
            // Adaptive backpressure when internal DRAM is tight (see constants):
            // slows the consumer so the SDIO streaming bursts — and their
            // assert-prone DMA alloc — stay small.  No-op on a roomy board.
            if (fi < OTA_DL_PACE_FLOOR) vTaskDelay(pdMS_TO_TICKS(OTA_DL_PACE_MS));
            int read = esp_https_ota_get_image_len_read(h);
            int pct  = (total > 0) ? (int)((int64_t)read * 100 / total) : 0;
            if (pct != last_pct) {
                last_pct = pct;
                p4DisplaySetOtaProgress(pct);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.progress = pct;
                xSemaphoreGive(s_lock);
                if (pct % 5 == 0) broadcast_status();
                if (pct % 10 == 0 && pct > 0) {
                    int64_t now = esp_timer_get_time();
                    double  dt  = (now - t_seg) / 1e6;          // s in this decile
                    int     db  = read - seg_bytes0;            // bytes in decile
                    if (dt > 0)
                        ESP_LOGI(TAG, "OTA %3d%%  segment %.1f KB/s  free_int=%u min=%u",
                                 pct, (db / 1024.0) / dt,
                                 (unsigned)fi, (unsigned)dl_min_int);
                    t_seg      = now;
                    seg_bytes0 = read;
                }
            }
        }
        ESP_LOGI(TAG, "OTA transfer min free internal DRAM = %u B", (unsigned)dl_min_int);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_perform failed: %s", esp_err_to_name(err));
            status_set_error("transfer error");
            esp_https_ota_abort(h);
            goto fail;
        }
        {
            double  secs  = (esp_timer_get_time() - t_start) / 1e6;
            int     bytes = esp_https_ota_get_image_len_read(h);
            ESP_LOGI(TAG, "OTA download: %d bytes in %.1f s = %.1f KB/s avg",
                     bytes, secs, secs > 0 ? (bytes / 1024.0) / secs : 0.0);
        }
        if (!esp_https_ota_is_complete_data_received(h)) {
            ESP_LOGE(TAG, "incomplete image");
            status_set_error("incomplete image");
            esp_https_ota_abort(h);
            goto fail;
        }

        err = esp_https_ota_finish(h);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_finish failed: %s", esp_err_to_name(err));
            status_set_error((err == ESP_ERR_OTA_VALIDATE_FAILED)
                                 ? "bad image" : "finalize failed");
            goto fail;
        }
    }

    ESP_LOGW(TAG, "OTA complete — rebooting into new image");
    p4DisplaySetOtaProgress(100);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();   // never returns

fail:
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.installing = false;
    s_status.progress   = 0;
    xSemaphoreGive(s_lock);
    s_installing.store(false);
    broadcast_status();
    // The running image is untouched by a failed download — return the LCD to
    // the normal UI rather than stranding it on the progress screen, and bring
    // the tunnel back so the device stays reachable.  Surface the failure on
    // the status bar (red) so the user sees why we bounced back to the main
    // screen instead of rebooting into a new image.
    p4DisplayHideOtaScreen();
    p4DisplaySetStatus(t(TK::OTA_FAILED), true);
    wstunnelApply();
    victronBleResume();
    ultimatronBleResume();
    vTaskDeleteWithCaps(nullptr);   // stack was allocated with xTaskCreateWithCaps
}

void p4OtaInstall() {
    // Single-flight: only one install at a time, and only when an update is
    // actually known.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_status.available && s_asset_url[0] && !s_status.installing;
    xSemaphoreGive(s_lock);
    if (!ok) {
        ESP_LOGW(TAG, "install requested but no update available / already running");
        return;
    }
    bool expected = false;
    if (!s_installing.compare_exchange_strong(expected, true)) return;

    // Stack in PSRAM, not internal DRAM.  xTaskCreate() forces task stacks into
    // internal RAM regardless of CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, and a
    // 32 KB *contiguous internal* block is often unavailable mid-run (WiFi/BLE/
    // tunnel/LVGL fragment it).  When that alloc failed, the ignored
    // xTaskCreate() return left s_installing stuck true forever — screen stuck
    // awake, checks skipped, install ignored.  WithCaps(SPIRAM) has room to
    // spare (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y); on failure we roll
    // the flag back so the device never strands.
    BaseType_t cr = xTaskCreateWithCaps(install_task, "p4_ota_install", 8192,
                                        nullptr, 4, nullptr,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (cr != pdPASS) {
        ESP_LOGE(TAG, "install task create failed (free_int=%u) — aborting",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        s_installing.store(false);
        status_set_error("low memory");
        broadcast_status();
    }
}

// ── Periodic check task ──────────────────────────────────────────────────
static void check_task(void*) {
    // Give WiFi a moment to associate, and let the BLE supervisor get a clean
    // scan window first (shared radio — see INITIAL_CHECK_DELAY_MS).
    vTaskDelay(pdMS_TO_TICKS(INITIAL_CHECK_DELAY_MS));
    for (;;) {
        bool manual = s_check_request.exchange(false);
        WifiStatus ws = wifi_manager_get_status();
        // Auto-checks honour the user's preference; an explicit request
        // (settings "Check" button / web) always runs.
        if (ws.connected && !s_installing.load() && (manual || p4OtaAutoCheckEnabled())) {
            do_check();
        }
        // Sleep in 1 s slices so p4OtaCheckNow() can interrupt the wait.
        for (uint32_t slept = 0; slept < CHECK_PERIOD_MS; slept += 1000) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (s_check_request.load()) break;   // consumed at loop top
            if (s_installing.load()) break;
        }
    }
}

void p4OtaCheckNow() { s_check_request.store(true); }

// ── Post-OTA self-test / rollback ────────────────────────────────────────
// Runs only when the running image is PENDING_VERIFY.  Distinguishes
// firmware-health signals (hard gates, environment-independent) from
// environment signals (best-effort).  See CLAUDE.md / the design notes.
static bool env_ready() {
    WifiStatus ws = wifi_manager_get_status();
    bool ip = ws.connected;

    // Tunnel: only required if the user enabled it.
    bool tun_ok = (wstunnelUiState() == TunnelUiState::DISABLED) ||
                  (wstunnelUiState() == TunnelUiState::CONNECTED);

    // LIN: a fresh slave frame (Combi powered & responding).
    TrumaLinSnapshot lin;
    trumaLinGetSnapshot(lin);
    bool lin_ok = lin.linOk;

    // BLE: if nothing is configured, treat as satisfied; otherwise require at
    // least one valid advert from any configured device.
    bool ble_configured = victronIsConfigured() || ultimatronIsConfigured() ||
                          tankIsConfigured() || multiplusIsConfigured();
    bool ble_ok = !ble_configured ||
                  victronGetData().valid || ultimatronGetData().valid ||
                  tankGetData().valid || multiplusGetData().valid;

    return ip && tun_ok && lin_ok && ble_ok;
}

// Persist why the post-OTA self-test rolled back, so the reason survives the
// reboot (the USB-Serial-JTAG console re-enumerates on reset, so the ESP_LOGE
// just before the rollback is usually lost).  Read + logged + cleared on the
// next boot by report_prior_rollback().
static void persist_rollback_reason(const char* why, uint32_t free_int) {
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "rb_why",  why);
    nvs_set_u32(h, "rb_heap", free_int);
    nvs_commit(h);
    nvs_close(h);
}

// Log (loudly) any rollback recorded by the previous boot's self-test, then
// clear it.  Called from p4OtaStart so a lost serial console no longer hides
// the reason — reconnect the monitor and it shows on the next boot.
static void report_prior_rollback() {
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    char why[48] = "";
    size_t len = sizeof(why);
    uint32_t heap = 0;
    if (nvs_get_str(h, "rb_why", why, &len) == ESP_OK) {
        nvs_get_u32(h, "rb_heap", &heap);
        ESP_LOGE(TAG, "PREVIOUS BOOT ROLLED BACK a self-OTA image: %s "
                      "(free internal DRAM was %lu B)", why, (unsigned long)heap);
        nvs_erase_key(h, "rb_why");
        nvs_erase_key(h, "rb_heap");
        nvs_commit(h);
    }
    nvs_close(h);
}

bool p4OtaPendingVerify() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

static void selftest_task(void*) {
    const esp_partition_t* run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK ||
        st != ESP_OTA_IMG_PENDING_VERIFY) {
        // Normal boot of an already-validated image — nothing to do.
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGW(TAG, "image is PENDING_VERIFY — running post-OTA self-test");

    uint32_t prev_beat[P4OTA_BEAT_COUNT];
    uint32_t last_advance_ms[P4OTA_BEAT_COUNT];
    uint32_t now0 = (uint32_t)(esp_timer_get_time() / 1000ULL);
    for (int i = 0; i < P4OTA_BEAT_COUNT; i++) {
        prev_beat[i]       = s_beats[i].load();
        last_advance_ms[i] = now0;
    }
    uint32_t t0 = now0;

    auto rollback = [](const char* why) {
        size_t fi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGE(TAG, "post-OTA self-test FAILED (%s, free_int=%u) — rolling back",
                 why, (unsigned)fi);
        persist_rollback_reason(why, (uint32_t)fi);
        esp_ota_mark_app_invalid_rollback_and_reboot();   // never returns
    };

    int heap_breach = 0;   // consecutive samples below the floor
    uint32_t last_log_ms = now0;
    size_t   min_free    = SIZE_MAX;   // lowest internal-DRAM sample seen

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SELFTEST_SAMPLE_MS));
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

        // Liveness/progress log every ~15 s so the silent PENDING_VERIFY wait
        // is visible and we can watch the real heap value while it runs.  The
        // min watermark is the number that actually matters for the floor.
        if (now - last_log_ms >= 15000) {
            last_log_ms = now;
            ESP_LOGI(TAG, "self-test running %lus — free_int=%u, min=%u, env_ready=%d",
                     (unsigned long)((now - t0) / 1000),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)min_free, (int)env_ready());
        }

        // Hard gate 1: internal DRAM floor — must be *sustained*.  A single
        // dip is normal (e.g. the WSS tunnel's TLS handshake right after boot
        // transiently needs internal DMA buffers); only a persistent breach
        // signals a real leak.  Require HEAP_BREACH_LIMIT consecutive samples.
        size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (free_int < min_free) min_free = free_int;
        if (free_int < HEAP_FLOOR) {
            if (++heap_breach >= HEAP_BREACH_LIMIT) rollback("heap floor breached");
        } else {
            heap_breach = 0;
        }

        // Hard gate 2: every critical task must keep scheduling.
        for (int i = 0; i < P4OTA_BEAT_COUNT; i++) {
            uint32_t b = s_beats[i].load();
            if (b != prev_beat[i]) {
                prev_beat[i]       = b;
                last_advance_ms[i] = now;
            } else if (now - last_advance_ms[i] > BEAT_STALL_MS) {
                char why[40];
                snprintf(why, sizeof(why), "task %d stalled", i);
                rollback(why);
            }
        }

        uint32_t elapsed = now - t0;
        // Fast path: healthy and the environment came up.
        if (elapsed >= SELFTEST_FAST_MIN_MS && env_ready()) {
            ESP_LOGI(TAG, "self-test passed (fast, %lus) — marking image valid",
                     (unsigned long)(elapsed / 1000));
            break;
        }
        // Ceiling: healthy long enough; validate regardless of environment so
        // a benign reset (Combi off, BLE out of range…) can't roll us back.
        if (elapsed >= SELFTEST_CEILING_MS) {
            ESP_LOGI(TAG, "self-test passed (ceiling, %lus) — marking image valid",
                     (unsigned long)(elapsed / 1000));
            break;
        }
    }

    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "image marked valid — rollback cancelled (min free_int during "
                  "self-test was %u B, floor %u B)",
             (unsigned)min_free, (unsigned)HEAP_FLOOR);

    // The WSS tunnel was deferred for this PENDING_VERIFY boot (its TLS
    // handshake is the heaviest internal-DRAM/SRAM consumer and, coinciding
    // with WiFi/BLE bring-up, is what sustained the heap below the floor).
    // Now the image is valid, so the handshake can no longer roll us back —
    // bring the tunnel up.  No-op if the tunnel is disabled in NVS.
    wstunnelApply();

    vTaskDelete(nullptr);
}

// ── Public entry ─────────────────────────────────────────────────────────
void p4OtaStart() {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    snprintf(s_status.currentVer, sizeof(s_status.currentVer), "%s", running_version());
    ESP_LOGI(TAG, "running firmware version: %s", s_status.currentVer);
    report_prior_rollback();   // surface any rollback from the previous boot

    nvs_handle_t h;
    if (nvs_open("ota", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 1;
        nvs_get_u8(h, "autocheck", &v);   // default 1 if absent
        s_autocheck = (v != 0);
        nvs_close(h);
    }

    xTaskCreate(selftest_task, "p4_ota_verify", 4096, nullptr, 6, nullptr);
    xTaskCreate(check_task,    "p4_ota_check",  6144, nullptr, 2, nullptr);
}
