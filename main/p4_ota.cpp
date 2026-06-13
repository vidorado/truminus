#include "p4_ota.hpp"
#include "version_compare.hpp"
#include "faultlog.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "esp_netif.h"
#include "nvs.h"
#include "miniz.h"            // ROM raw-DEFLATE inflate (tinfl_*) for littlefs.bin.gz

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
// Marker asset that promotes an otherwise-silent patch to an auto-prompt: a
// release that should actively notify (e.g. a security/important fix) includes a
// zero-byte asset with this name. SemVer has no "importance" field, so the
// signal lives out-of-band in the release. See release_has_force_notify().
#define OTA_FORCE_NOTIFY_ASSET "force-notify"

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

// Install-task stack, in BYTES (IDF: StackType_t == uint8_t).  Must come from
// INTERNAL DRAM (dynamic xTaskCreate): esp_https_ota writes flash with the SPI
// cache — and thus PSRAM — disabled, so a PSRAM stack is unreachable then
// (esp_task_stack_is_sane_cache_disabled panic).  Reserving it statically in
// .bss instead starved the idle task at boot, so it stays dynamic and we just
// handle a creation failure gracefully.
static constexpr uint32_t INSTALL_STACK_BYTES = 8192;     // 8 KB

// Heartbeat counters bumped by the critical tasks.
static std::atomic<uint32_t> s_beats[P4OTA_BEAT_COUNT];

// Set by p4OtaCheckNow() to interrupt the check task's sleep.
static std::atomic<bool> s_check_request{false};

// Set by p4OtaCancel() to abort an in-progress install (checked in the perform
// loop).  Cleared at the start of each install.
static std::atomic<bool> s_cancel{false};

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
             "{\"command\":\"ota\",\"checking\":%s,\"available\":%s,\"installing\":%s,"
             "\"progress\":%d,\"current\":\"%s\",\"latest\":\"%s\",\"error\":\"%s\"}",
             st.checking ? "true" : "false",
             st.available ? "true" : "false",
             st.installing ? "true" : "false",
             st.progress, st.currentVer, st.latestVer, st.error);
    wsQueueSend(buf);
}

// ── Version helpers ──────────────────────────────────────────────────────
// parse_semver / semver_cmp / is_minor_or_major_newer live in the IDF-free
// version_compare.{hpp,cpp} module so they can be host-tested against the real
// code (test/host/test_semver.cpp). Note on the gating policy: the proactive
// auto-notification (topbar reminder icon + prompt modal) uses
// is_minor_or_major_newer so patch releases don't nag the user, while manual
// checks (settings screen / `ota check`) and install use plain semver_cmp, so
// patches are still surfaced and installable when explicitly sought.

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
    // Short timeout so a check on a marginal WiFi link fails fast and stops
    // hogging airtime from the WSS tunnel instead of blocking for 10 s. The
    // auto-check is a convenience; a missed run just retries on the next cycle.
    cfg.timeout_ms            = 6000;
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

// True if the release tagged `tag` carries the OTA_FORCE_NOTIFY_ASSET marker.
// GitHub 302-redirects an existing asset's download URL to the CDN and 404s a
// missing one, so a single header-only request (no body, no GitHub API/auth)
// answers it — the same redirect trick fetch_latest_tag() uses. Only called when
// an update is already available, so the extra request is rare.
static bool release_has_force_notify(const char* tag) {
    char url[256];
    snprintf(url, sizeof(url),
             "https://github.com/" OTA_GH_OWNER "/" OTA_GH_REPO
             "/releases/download/%s/" OTA_FORCE_NOTIFY_ASSET, tag);

    esp_http_client_config_t cfg = {};
    cfg.url                   = url;
    cfg.crt_bundle_attach     = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = true;       // we only want the status, not the asset
    cfg.timeout_ms            = 6000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_http_client_set_header(c, "User-Agent", "TruMinus-OTA");

    bool exists = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int st = esp_http_client_get_status_code(c);
        exists = (st == 301 || st == 302 || st == 307 || st == 308);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return exists;
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

    // The suspend is only honored at the supervisor's scan-window boundary, so
    // wait (≤6 s) for any in-flight scan to end before fetching — otherwise the
    // check overlaps a live scan on the shared C6 radio.  The physical Updates
    // screen avoids this naturally by pausing BLE on entry (lead time).
    for (int i = 0; i < 60 && victronBleScanActive(); i++) vTaskDelay(pdMS_TO_TICKS(100));

    char tag[32] = "";
    bool got = fetch_latest_tag(tag, sizeof(tag));

    // Decide notify-worthiness while BLE is still paused (shared radio). A patch
    // is normally silent; a release can force the prompt by shipping the
    // force-notify marker asset. Both network probes run before resuming BLE.
    bool newer  = got && semver_cmp(tag, running_version()) > 0;
    bool forced = newer && release_has_force_notify(tag);

    if (!vicWas) victronBleResume();
    if (!ultWas) ultimatronBleResume();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.checking = false;
    if (!got) {
        snprintf(s_status.error, sizeof(s_status.error), "check failed");
    } else {
        s_status.error[0] = '\0';
        bool mm = newer && is_minor_or_major_newer(tag, s_status.currentVer);
        s_status.available = newer;
        s_notify_worthy = newer && (mm || forced);
        if (newer) {
            snprintf(s_status.latestVer, sizeof(s_status.latestVer), "%s", tag);
            snprintf(s_asset_url, sizeof(s_asset_url),
                     "https://github.com/" OTA_GH_OWNER "/" OTA_GH_REPO
                     "/releases/download/%s/" OTA_ASSET, tag);
            const char* why = !s_notify_worthy ? "patch — silent (manual check only)"
                            : mm               ? "minor/major — will prompt"
                                               : "patch — forced prompt (marker asset)";
            ESP_LOGI(TAG, "update available: %s -> %s (%s)", s_status.currentVer, tag, why);
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
    s_cancel.store(false);   // fresh install — clear any stale cancel request
    broadcast_status();

    ESP_LOGI(TAG, "starting self-OTA from %s", url);
    p4DisplayShowOtaScreen(cur, latest);

    // NimBLE was fully torn down by install_prep_task, freeing the tens of KB
    // of internal DRAM the esp_hosted SDIO RX path needs (it hard-asserts in
    // sdio_rx_get_buffer when its per-burst DMA alloc fails).  That headroom is
    // what lets us keep the WSS tunnel UP through the download, so remote clients
    // can watch the progress bar.  Everything returns on the reboot that ends
    // every install (success OR failure).
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
            if (s_cancel.load()) {
                ESP_LOGW(TAG, "install cancelled by user");
                status_set_error("cancelled");
                esp_https_ota_abort(h);
                goto fail;
            }
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
    broadcast_status();
    // NimBLE was torn down for headroom and can't be cleanly re-inited from
    // here — reboot to restore BLE (and a clean state).  Show the failure
    // briefly first so it's visible before the restart.
    ESP_LOGW(TAG, "OTA failed — rebooting to restore BLE/services");
    p4DisplaySetStatus(t(TK::OTA_FAILED), true);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();   // never returns
}

// Orchestrator (own task, off the LVGL/WS caller context since the NimBLE
// teardown blocks): frees internal DRAM by tearing NimBLE down entirely, THEN
// spawns the install task.  Freeing NimBLE's tens of KB gives the SDIO RX path
// the headroom it needs AND lets the tunnel stay up for remote progress.
static void install_prep_task(void*) {
    ESP_LOGI(TAG, "install prep: free_int=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    bool ble_ok = bleSupervisorStop();   // NimBLEDevice::deinit(false) — frees the host RAM

    ESP_LOGI(TAG, "install prep done (ble_ok=%d): free_int=%u largest=%u (need %u)",
             (int)ble_ok,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)INSTALL_STACK_BYTES);

    if (!ble_ok) {
        ESP_LOGE(TAG, "BLE teardown failed — rebooting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    BaseType_t cr = xTaskCreate(install_task, "p4_ota_install",
                                INSTALL_STACK_BYTES, nullptr, 4, nullptr);
    if (cr != pdPASS) {
        // NimBLE is already down; a clean restart is the only sane recovery.
        ESP_LOGE(TAG, "install task create failed after freeing — rebooting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    vTaskDelete(nullptr);
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

    // Small prep task frees DRAM (NimBLE teardown) then spawns the real install
    // task.  A modest stack so it allocates even with the board's tight internal.
    BaseType_t cr = xTaskCreate(install_prep_task, "p4_ota_prep",
                                4096, nullptr, 4, nullptr);
    if (cr != pdPASS) {
        ESP_LOGE(TAG, "install prep create failed — aborting");
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

// Request abort of an in-progress install (no-op if none).  The install task
// notices in its perform loop, aborts the OTA and returns to the normal UI.
void p4OtaCancel() { if (s_installing.load()) s_cancel.store(true); }

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

// ── LittleFS (web assets) sync ───────────────────────────────────────────
// The app OTA (esp_https_ota → truminus.bin) updates only the application
// partition; the web UI lives on the separate `littlefs` data partition. After
// an app image is validated we bring the web in line: compare the content hash
// baked into our image (/littlefs/fs.ver) with the release's `littlefs.ver`
// marker; on a mismatch download `littlefs.bin.gz` (~500 KB — the 8 MB image is
// ~95% 0xFF so it gzips tiny), inflate it and rewrite the partition. Tied to the
// app update because every release bumps the version, so a web-only change still
// ships as a new release the device installs.
#define OTA_FS_ASSET     "littlefs.bin.gz"
#define OTA_FS_VER_ASSET "littlefs.ver"
static constexpr size_t OTA_FS_GZ_MAX = 2 * 1024 * 1024;   // sanity cap on the .gz

// The release tag that hosts the assets: the running version with any
// "-g<sha>"/"-dirty" suffix stripped. A clean OTA'd image is already "X.Y.Z".
static void release_tag_of_running(char* out, size_t out_len) {
    snprintf(out, out_len, "%s", running_version());
    char* dash = strchr(out, '-');
    if (dash) *dash = '\0';
}

static void broadcast_fsupdate(const char* state, int pct) {
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"command\":\"fsupdate\",\"state\":\"%s\",\"pct\":%d}", state, pct);
    wsQueueSend(buf);
}

// Read /littlefs/fs.ver — the content hash baked into our current web image.
static bool littlefs_read_ver(char* out, size_t out_len) {
    FILE* f = fopen("/littlefs/fs.ver", "r");
    if (!f) return false;
    char raw[96] = "";
    size_t n = fread(raw, 1, sizeof(raw) - 1, f);
    fclose(f);
    raw[n] = '\0';
    for (size_t e = strlen(raw); e && (raw[e-1]=='\n'||raw[e-1]=='\r'||raw[e-1]==' '); )
        raw[--e] = '\0';
    snprintf(out, out_len, "%s", raw);
    return out[0] != '\0';
}

// Public: short web-assets version (first 12 hex of /littlefs/fs.ver) for the
// LCD Updates screen + web About overlay.  "—" when unavailable (no marker yet
// / mid-sync unmount).
void p4OtaFsVersion(char* out, size_t out_len) {
    char full[96] = "";
    if (littlefs_read_ver(full, sizeof(full)) && full[0])
        snprintf(out, out_len, "%.12s", full);
    else
        snprintf(out, out_len, "—");
}

// Open `c` and follow GitHub's release-asset redirect (302 → CDN) up to a few
// hops, leaving the client positioned to read the final 200 body.  The manual
// open/fetch_headers flow (unlike esp_http_client_perform / esp_https_ota) does
// NOT auto-follow redirects, so we drive it explicitly.  Returns the final HTTP
// status, or -1 on a transport error.
static int http_open_follow(esp_http_client_handle_t c) {
    for (int hop = 0; hop < 6; hop++) {
        if (esp_http_client_open(c, 0) != ESP_OK) return -1;
        esp_http_client_fetch_headers(c);
        int st = esp_http_client_get_status_code(c);
        if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
            esp_http_client_set_redirection(c);   // adopt the Location header
            esp_http_client_close(c);
            continue;
        }
        return st;
    }
    return -1;
}

// Outcome of fetching the littlefs.ver marker.  ABSENT (the release carries no
// marker — a pre-feature release) and ERROR (network/transport/CDN failure)
// must be handled differently by the caller: ABSENT is a permanent "nothing to
// do", ERROR is transient and worth retrying on the next boot.
enum class FsVerResult { OK, ABSENT, ERROR };

// GET the tiny `littlefs.ver` asset body for `tag` (follows the CDN redirect).
static FsVerResult fetch_fs_ver(const char* tag, char* out, size_t out_len) {
    char url[256];
    snprintf(url, sizeof(url),
             "https://github.com/" OTA_GH_OWNER "/" OTA_GH_REPO
             "/releases/download/%s/" OTA_FS_VER_ASSET, tag);
    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms        = 8000;
    cfg.buffer_size       = 2048;        // CDN response headers
    cfg.buffer_size_tx    = 4096;        // request line carries the long signed CDN URL
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return FsVerResult::ERROR;
    esp_http_client_set_header(c, "User-Agent", "TruMinus-OTA");
    int  status = http_open_follow(c);
    bool body_ok = false;
    if (status == 200) {
        char raw[96] = "";
        int r = esp_http_client_read_response(c, raw, sizeof(raw) - 1);
        if (r > 0) {
            raw[r] = '\0';
            for (size_t e = strlen(raw); e && (raw[e-1]=='\n'||raw[e-1]=='\r'||raw[e-1]==' '); )
                raw[--e] = '\0';
            snprintf(out, out_len, "%s", raw);
            body_ok = (out[0] != '\0');
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (status == 200) return body_ok ? FsVerResult::OK : FsVerResult::ERROR;
    if (status == 404) return FsVerResult::ABSENT;   // release predates the feature
    return FsVerResult::ERROR;                        // -1 transport / unexpected status
}

// Download littlefs.bin.gz for `tag` into a fresh PSRAM buffer.  Pushes download
// progress to the LCD + web as it goes.
static bool download_fs_gz(const char* tag, uint8_t** out_buf, size_t* out_len) {
    char url[256];
    snprintf(url, sizeof(url),
             "https://github.com/" OTA_GH_OWNER "/" OTA_GH_REPO
             "/releases/download/%s/" OTA_FS_ASSET, tag);
    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms        = 20000;
    cfg.buffer_size       = 16 * 1024;   // full TLS record per read
    cfg.buffer_size_tx    = 4096;        // request line carries the long signed CDN URL
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_http_client_set_header(c, "User-Agent", "TruMinus-OTA");

    uint8_t* buf = nullptr;
    size_t len = 0, cap = 0;
    bool ok = false;
    int status = http_open_follow(c);
    if (status == 200) {
        int total = (int)esp_http_client_get_content_length(c);
        {
            cap = (total > 0 && (size_t)total <= OTA_FS_GZ_MAX) ? (size_t)total : 512 * 1024;
            buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
            while (buf && len < OTA_FS_GZ_MAX) {
                if (len == cap) {                       // short/absent Content-Length
                    size_t ncap = cap < OTA_FS_GZ_MAX/2 ? cap * 2 : OTA_FS_GZ_MAX;
                    uint8_t* nb = (uint8_t*)heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM);
                    if (!nb) break;
                    buf = nb; cap = ncap;
                }
                int r = esp_http_client_read(c, (char*)buf + len, cap - len);
                if (r < 0) { len = 0; break; }
                if (r == 0) { ok = esp_http_client_is_complete_data_received(c); break; }
                len += r;
                int pct = (total > 0) ? (int)((uint64_t)len * 100 / (size_t)total) : 50;
                broadcast_fsupdate("downloading", pct);
                p4DisplaySetOtaProgress(pct);
            }
        }
    } else {
        ESP_LOGW(TAG, "fs gz status %d", status);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (ok && buf && len > 18) { *out_buf = buf; *out_len = len; return true; }
    if (buf) heap_caps_free(buf);
    return false;
}

// NVS retry flag: a failed/interrupted sync is retried on the next boot.
static void fs_pending_set(const char* tag) {
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "fs_pending", tag); nvs_commit(h); nvs_close(h);
}
static void fs_pending_clear() {
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "fs_pending"); nvs_commit(h); nvs_close(h);
}

// Inflate `gz` (raw DEFLATE after the gzip header) straight into the littlefs
// partition, streaming through tinfl's 32 KB ring dictionary.  Do NOT buffer
// the full 8 MB image: PSRAM is plentiful but a single 8 MB *contiguous* block
// is not guaranteed once the heap has churned for a while ("alloc failed" seen
// in the field).  Flash writes go through a small INTERNAL bounce buffer — the
// only memory esp_partition_write touches while the cache is disabled.
// Progress: erase sweeps 0–10 %, writing 10–100 %.
static bool inflate_to_partition(const esp_partition_t* part,
                                 const uint8_t* deflate, size_t deflate_len) {
    bool ok = false;
    uint8_t* dict = (uint8_t*)heap_caps_malloc(TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM);
    auto*    dec  = (tinfl_decompressor*)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
    uint8_t* bnc  = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL);
    if (!dict || !dec || !bnc) {
        ESP_LOGE(TAG, "fs inflate: alloc failed (dict=%p dec=%p bnc=%p | "
                 "SPIRAM free=%u largest=%u | INT free=%u largest=%u)",
                 dict, dec, bnc,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        goto done;
    }

    {
        // Erase in chunks so the progress bar moves instead of freezing for the
        // ~10 s a whole-partition erase takes.
        for (size_t off = 0; off < part->size; ) {
            size_t n = part->size - off; if (n > 512 * 1024) n = 512 * 1024;
            if (esp_partition_erase_range(part, off, n) != ESP_OK) {
                ESP_LOGE(TAG, "fs inflate: erase @%u failed", (unsigned)off);
                goto done;
            }
            off += n;
            int pct = (int)((uint64_t)off * 10 / part->size);
            broadcast_fsupdate("flashing", pct);
            p4DisplaySetOtaProgress(pct);
        }

        tinfl_init(dec);
        size_t in_pos = 0, written = 0, dict_ofs = 0;
        for (;;) {
            size_t in_avail  = deflate_len - in_pos;
            size_t out_avail = TINFL_LZ_DICT_SIZE - dict_ofs;
            tinfl_status st = tinfl_decompress(dec, deflate + in_pos, &in_avail,
                                               dict, dict + dict_ofs, &out_avail, 0);
            in_pos += in_avail;
            if (written + out_avail > part->size) {
                ESP_LOGE(TAG, "fs inflate: image larger than partition");
                goto done;
            }
            // Flush the freshly produced ring segment to flash.
            for (size_t o = 0; o < out_avail; ) {
                size_t n = out_avail - o; if (n > 4096) n = 4096;
                memcpy(bnc, dict + dict_ofs + o, n);    // PSRAM → internal (cache on)
                if (esp_partition_write(part, written + o, bnc, n) != ESP_OK) {
                    ESP_LOGE(TAG, "fs inflate: write @%u failed", (unsigned)(written + o));
                    goto done;
                }
                o += n;
            }
            written  += out_avail;
            dict_ofs  = (dict_ofs + out_avail) & (TINFL_LZ_DICT_SIZE - 1);
            int pct = 10 + (int)((uint64_t)written * 90 / part->size);
            broadcast_fsupdate("flashing", pct);
            p4DisplaySetOtaProgress(pct);
            if (st == TINFL_STATUS_DONE) break;
            if (st != TINFL_STATUS_HAS_MORE_OUTPUT) {
                ESP_LOGE(TAG, "fs inflate: tinfl st=%d at %u/%u",
                         (int)st, (unsigned)written, (unsigned)part->size);
                goto done;
            }
        }
        if (written != part->size) {
            ESP_LOGE(TAG, "fs inflate: short image %u/%u",
                     (unsigned)written, (unsigned)part->size);
            goto done;
        }
        ok = true;
    }
done:
    if (dict) heap_caps_free(dict);
    if (dec)  heap_caps_free(dec);
    if (bnc)  heap_caps_free(bnc);
    return ok;
}

// Bring the web assets in line with release `tag`.  Cheap no-op when our
// /littlefs/fs.ver already matches the release marker.
static void littlefs_sync(const char* tag) {
    char cur[96] = "", want[96] = "";
    bool have_cur = littlefs_read_ver(cur, sizeof(cur));
    FsVerResult fr = fetch_fs_ver(tag, want, sizeof(want));
    if (fr == FsVerResult::ABSENT) {
        // The release genuinely carries no marker (predates the feature).
        // Nothing this device can do — clear any stale pending so we don't
        // retry a sync that can never complete.
        ESP_LOGW(TAG, "fs sync: no littlefs.ver on %s — skipping (pre-feature release)", tag);
        fs_pending_clear();
        return;
    }
    if (fr == FsVerResult::ERROR) {
        // Transient: DNS/TLS/CDN hiccup, common right after a PENDING_VERIFY
        // boot when the network is still settling.  Persist the intent so the
        // boot-time retry in p4OtaStart() picks it up once WiFi is stable —
        // previously this path returned silently and the sync was lost until
        // the next OTA.
        ESP_LOGW(TAG, "fs sync: fetch of littlefs.ver for %s failed — will retry next boot", tag);
        fs_pending_set(tag);
        return;
    }
    if (have_cur && strcmp(cur, want) == 0) {
        ESP_LOGI(TAG, "fs sync: web up to date (%s)", cur);
        fs_pending_clear();
        return;
    }
    ESP_LOGW(TAG, "fs sync: web differs (have=%s want=%s) — updating",
             have_cur ? cur : "<none>", want);

    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "littlefs");
    if (!part) { ESP_LOGE(TAG, "fs sync: no littlefs partition"); return; }

    fs_pending_set(tag);                       // cleared only on full success
    s_installing.store(true);                  // keeps the LCD awake, parks the checker
    char fromv[16], tov[16];
    snprintf(fromv, sizeof(fromv), "%.12s", have_cur ? cur : "\xE2\x80\x94");
    snprintf(tov,   sizeof(tov),   "%.12s", want);
    p4DisplayShowOtaScreen(fromv, tov, t(TK::OTA_WEB_UPDATING), false);
    broadcast_fsupdate("downloading", 0);

    // Free the shared C6 radio for the download (BLE rides the same radio).
    bool vicWas = victronBleSuspended(), ultWas = ultimatronBleSuspended();
    if (!vicWas) victronBleSuspend();
    if (!ultWas) ultimatronBleSuspend();

    uint8_t* gz = nullptr; size_t gzlen = 0;
    bool dl = download_fs_gz(tag, &gz, &gzlen);

    if (!vicWas) victronBleResume();
    if (!ultWas) ultimatronBleResume();

    auto fail = [&](const char* why) {
        ESP_LOGE(TAG, "fs sync: %s — will retry next boot", why);
        if (gz) heap_caps_free(gz);
        broadcast_fsupdate("error", 0);
        p4DisplaySetStatus(t(TK::OTA_WEB_FAILED), true);
        p4DisplayHideOtaScreen();
        s_installing.store(false);
    };
    if (!dl) { fail("download failed"); return; }

    // gzip header: with `gzip -n` it is a fixed 10 bytes (magic 1f 8b, method 08,
    // FLG 0, mtime 0).  Validate, tolerate an optional FNAME, then inflate the
    // raw DEFLATE that follows.
    size_t hdr = 10;
    if (gz[0] != 0x1f || gz[1] != 0x8b || gz[2] != 0x08 || (gz[3] & 0xe0) != 0) {
        fail("bad gzip header"); return;
    }
    if (gz[3] & 0x08) { while (hdr < gzlen && gz[hdr]) hdr++; hdr++; }   // skip FNAME
    if (hdr + 8 >= gzlen) { fail("truncated gzip"); return; }

    // Unmount → erase+inflate+write → remount.  The WS server stays up (only
    // static-file serving is down), so flashing progress keeps streaming.
    broadcast_fsupdate("flashing", 0);
    unmountWebFs();
    bool wrote = inflate_to_partition(part, gz + hdr, gzlen - hdr);
    heap_caps_free(gz);
    if (mountWebFs() != ESP_OK)
        ESP_LOGE(TAG, "fs sync: remount failed (web down until next boot)");

    if (wrote) {
        fs_pending_clear();
        ESP_LOGW(TAG, "fs sync: web updated to %s", want);
        broadcast_fsupdate("done", 100);
        p4DisplayHideOtaScreen();
    } else {
        broadcast_fsupdate("error", 0);                // fs_pending stays set
        p4DisplaySetStatus(t(TK::OTA_WEB_FAILED), true);
        p4DisplayHideOtaScreen();
    }
    s_installing.store(false);
}

static void fs_sync_task(void* arg) {
    char* tag = (char*)arg;
    // The boot-time fs_pending retry fires from p4OtaStart() while WiFi may
    // still be associating — wait for an IP (up to 60 s) so fetch_fs_ver
    // doesn't fail DNS and silently skip the sync until the next boot.
    for (int i = 0; i < 60; i++) {
        esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip = {};
        if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    littlefs_sync(tag);
    free(tag);
    vTaskDelete(nullptr);
}

// Spawn the web-asset sync off the caller's context (it blocks on network +
// flash).  Stack stays modest: the heavy inflate buffers live in PSRAM/heap.
static void littlefs_sync_async(const char* tag) {
    char* dup = strdup(tag);
    if (!dup) return;
    if (xTaskCreate(fs_sync_task, "p4_fs_sync", 6144, dup, 4, nullptr) != pdPASS) free(dup);
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
        // Also record into the faultlog "diag" slot so the reason shows in the
        // web About overlay — including on the OLDER image we roll back into.
        faultLogRecordRollback(why, (uint32_t)fi);
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
    faultLogClearRollback();   // successful update — drop any stale rollback note
    ESP_LOGI(TAG, "image marked valid — rollback cancelled (min free_int during "
                  "self-test was %u B, floor %u B)",
             (unsigned)min_free, (unsigned)HEAP_FLOOR);

    // The WSS tunnel was deferred for this PENDING_VERIFY boot (its TLS
    // handshake is the heaviest internal-DRAM/SRAM consumer and, coinciding
    // with WiFi/BLE bring-up, is what sustained the heap below the floor).
    // Now the image is valid, so the handshake can no longer roll us back —
    // bring the tunnel up.  No-op if the tunnel is disabled in NVS.
    wstunnelApply();

    // App image is good — bring the web assets (LittleFS) in line with this
    // release.  Cheap no-op when /littlefs/fs.ver already matches the marker.
    char fs_tag[32];
    release_tag_of_running(fs_tag, sizeof(fs_tag));
    littlefs_sync_async(fs_tag);

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

    // Retry a web-asset sync left pending by a prior failed/interrupted attempt.
    // Only on a normal boot — a PENDING_VERIFY boot's self-test triggers the
    // sync after validating, so this avoids double-running it.
    if (!p4OtaPendingVerify()) {
        nvs_handle_t fh;
        if (nvs_open("ota", NVS_READONLY, &fh) == ESP_OK) {
            char ptag[32] = "";
            size_t pl = sizeof(ptag);
            if (nvs_get_str(fh, "fs_pending", ptag, &pl) == ESP_OK && ptag[0])
                littlefs_sync_async(ptag);
            nvs_close(fh);
        }
    }

    xTaskCreate(selftest_task, "p4_ota_verify", 4096, nullptr, 6, nullptr);
    // Lowest practical priority: the version check is background convenience and
    // must yield CPU to the WiFi/tunnel tasks (the real contention is airtime,
    // but keeping it below everything networking-related costs nothing).
    xTaskCreate(check_task,    "p4_ota_check",  6144, nullptr, 1, nullptr);
}
