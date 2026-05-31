#include "p4_ota.hpp"

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
#include "nvs.h"

#include "p4display.hpp"
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

// ── Post-OTA self-test gating ───────────────────────────────────────────
// Hard firmware-health gates (independent of the environment):
static constexpr size_t   HEAP_FLOOR        = 24 * 1024;  // internal DRAM
static constexpr uint32_t BEAT_STALL_MS     = 20000;      // task considered dead
// Validation timing:
static constexpr uint32_t SELFTEST_FAST_MIN_MS = 90  * 1000;   // earliest validate
static constexpr uint32_t SELFTEST_CEILING_MS  = 480 * 1000;   // validate regardless
static constexpr uint32_t SELFTEST_SAMPLE_MS   = 2000;

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
    bool v = s_status.available && s_autocheck;
    xSemaphoreGive(s_lock);
    return v;
}

bool p4OtaPromptPending(char* cur, size_t cur_len, char* latest, size_t latest_len) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool pend = s_status.available && s_autocheck &&
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

    char tag[32] = "";
    bool got = fetch_latest_tag(tag, sizeof(tag));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.checking = false;
    if (!got) {
        snprintf(s_status.error, sizeof(s_status.error), "check failed");
    } else {
        s_status.error[0] = '\0';
        bool newer = semver_cmp(tag, s_status.currentVer) > 0;
        s_status.available = newer;
        if (newer) {
            snprintf(s_status.latestVer, sizeof(s_status.latestVer), "%s", tag);
            snprintf(s_asset_url, sizeof(s_asset_url),
                     "https://github.com/" OTA_GH_OWNER "/" OTA_GH_REPO
                     "/releases/download/%s/" OTA_ASSET, tag);
            ESP_LOGI(TAG, "update available: %s -> %s", s_status.currentVer, tag);
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

    // Free internal DRAM for the TLS handshake: tear the WSS tunnel down for
    // the duration.  The reboot on success re-establishes it; on failure we
    // re-apply the saved config below.
    wstunnelSuspend();
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
    // buffer" and esp_https_ota_begin() fails.  Give it room.
    http.buffer_size       = 4096;
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
        while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int read = esp_https_ota_get_image_len_read(h);
            int pct  = (total > 0) ? (int)((int64_t)read * 100 / total) : 0;
            if (pct != last_pct) {
                last_pct = pct;
                p4DisplaySetOtaProgress(pct);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.progress = pct;
                xSemaphoreGive(s_lock);
                if (pct % 5 == 0) broadcast_status();
            }
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_perform failed: %s", esp_err_to_name(err));
            status_set_error("transfer error");
            esp_https_ota_abort(h);
            goto fail;
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
    // the tunnel back so the device stays reachable.
    p4DisplayHideOtaScreen();
    wstunnelApply();
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
    xTaskCreate(install_task, "p4_ota_install", 8192, nullptr, 4, nullptr);
}

// ── Periodic check task ──────────────────────────────────────────────────
static void check_task(void*) {
    // Give WiFi a moment to associate before the first check.
    vTaskDelay(pdMS_TO_TICKS(8000));
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
        ESP_LOGE(TAG, "post-OTA self-test FAILED (%s) — rolling back", why);
        esp_ota_mark_app_invalid_rollback_and_reboot();   // never returns
    };

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SELFTEST_SAMPLE_MS));
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

        // Hard gate 1: internal DRAM floor.
        size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (free_int < HEAP_FLOOR) {
            rollback("heap floor breached");
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
    ESP_LOGI(TAG, "image marked valid — rollback cancelled");
    vTaskDelete(nullptr);
}

// ── Public entry ─────────────────────────────────────────────────────────
void p4OtaStart() {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    snprintf(s_status.currentVer, sizeof(s_status.currentVer), "%s", running_version());
    ESP_LOGI(TAG, "running firmware version: %s", s_status.currentVer);

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
