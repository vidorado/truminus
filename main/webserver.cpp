#include "webserver.hpp"
#include "logs.hpp"

#ifdef WEBSERVER

#include <atomic>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/param.h>          // MIN / MAX
#include <sys/socket.h>         // shutdown / close
#include <unistd.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_heap_caps.h"

// ── Globals ──────────────────────────────────────────────────────────────────

// Two separate httpd instances so a burst of parallel asset GETs through the
// reverse tunnel (`wstunnel.cpp`) can never evict an existing /ws client via
// the per-server LRU.  `httpServer` (port 80) serves every static asset.
// `wsServer`   (port 81) is reachable only via loopback and is dedicated to
// WebSocket sessions; `wstunnel.cpp::try_open_locked` peeks the first chunk
// of each tunneled stream and routes `GET /ws` to :81, everything else to
// :80.  The two LRU pools are independent, so a 12-asset stampede cannot
// touch the WS pool.
httpd_handle_t   httpServer    = nullptr;
httpd_handle_t   wsServer      = nullptr;
QueueHandle_t    wsQueue       = nullptr;
std::atomic<int> wsClientCount{0};

static WsCommandCb    s_cmdCb  = nullptr;
static WsConnectedCb s_connCb = nullptr;
static bool           s_lfsMounted = false;

// Cross-task WebSocket broadcast queue.
//   WS_QUEUE_LEN  — slots.  Initial-state burst is ~30 messages; size 48
//                   keeps headroom for LIN/BLE updates piling in while
//                   wsQueueDrain() is mid-drain.
//   WS_QUEUE_MSG  — max payload bytes per message.  All current snapshots
//                   (setpoint changes, frame publishers, snapshot JSON) fit
//                   well under 256 B; reserve 256 to absorb future growth.
static constexpr UBaseType_t WS_QUEUE_LEN = 48;
static constexpr UBaseType_t WS_QUEUE_MSG = 256;

// Cap on simultaneous WebSocket clients.  esp_http_server reserves one task
// slot per concurrent request; each open WS uses ~6 KB of internal SRAM
// (control block + recv buffer).  4 leaves room for one reload overlap on a
// machine that already has BLE + WiFi-via-C6 + LVGL running.
static constexpr uint8_t WS_MAX_CLIENTS = 8;

// Maximum sockets per httpd instance.  Must match `httpd_config_t::
// max_open_sockets` exactly for each server — `httpd_get_client_list()`
// rejects any smaller buffer with ESP_ERR_INVALID_ARG (silently breaks
// broadcast).
//
// _HTTP : static-asset pool.  Sized for the worst-case page-load burst
//         (HTML + CSS + JS + fonts + favicon + assorted images).
// _WS   : WebSocket pool, isolated.  Each httpd socket pre-allocates
//         CONFIG_HTTPD_MAX_REQ_HDR_LEN (4 KB) of scratch, so going as
//         wide as WS_MAX_CLIENTS=8 costs 32 KB of DRAM that mbedtls/PSA
//         then can't use to verify the WSS cert at boot.  4 slots cover
//         the realistic "a couple of tabs over the tunnel" case; LAN-direct
//         WS clients hit :80 anyway.
static constexpr size_t MAX_OPEN_SOCKETS_HTTP = 12;
static constexpr size_t MAX_OPEN_SOCKETS_WS   = 4;

static const char* TAG = "web";

// ── LittleFS mount ───────────────────────────────────────────────────────────

esp_err_t mountWebFs() {
    if (s_lfsMounted) return ESP_OK;

    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path              = "/littlefs";
    conf.partition_label        = "littlefs";
    conf.format_if_mount_failed = false;
    conf.dont_mount             = false;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted: %u / %u bytes used",
             (unsigned)used, (unsigned)total);

    s_lfsMounted = true;
    return ESP_OK;
}

// ── HTTP static file serving ─────────────────────────────────────────────────

// MIME table.  Anything not listed falls back to application/octet-stream.
struct MimeEntry { const char* ext; const char* type; };
static const MimeEntry s_mime[] = {
    {".html", "text/html; charset=utf-8"},
    {".htm",  "text/html; charset=utf-8"},
    {".css",  "text/css; charset=utf-8"},
    {".js",   "application/javascript; charset=utf-8"},
    {".json", "application/json"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".svg",  "image/svg+xml"},
    {".ico",  "image/x-icon"},
    {".woff", "font/woff"},
    {".woff2","font/woff2"},
    {".ttf",  "font/ttf"},
    {".txt",  "text/plain; charset=utf-8"},
};

static const char* mimeOf(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    for (const auto& m : s_mime) {
        if (strcasecmp(dot, m.ext) == 0) return m.type;
    }
    return "application/octet-stream";
}

// Serve a file from /littlefs/<rel>, streaming in 4 KB chunks to keep peak
// heap usage flat regardless of asset size.  Adds Cache-Control: immutable
// to match the cache_bust.py querystring strategy.  If the asset is gzipped
// on disk (a sibling .gz exists), the gzipped variant is preferred and
// Content-Encoding is set automatically.
static esp_err_t serveFile(httpd_req_t* req, const char* relpath) {
    char fs_path[160];
    snprintf(fs_path, sizeof(fs_path), "/littlefs/%s", relpath);

    // Prefer pre-gzipped variant if present (e.g. script.js.gz).
    char gz_path[170];
    snprintf(gz_path, sizeof(gz_path), "%s.gz", fs_path);

    bool   gzipped = false;
    FILE*  f       = fopen(gz_path, "rb");
    if (f) {
        gzipped = true;
    } else {
        f = fopen(fs_path, "rb");
    }
    if (!f) {
        ESP_LOGW(TAG, "404: %s", fs_path);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "Not Found", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, mimeOf(relpath));
    // The HTML entry document is requested at "/" without a cache-busting
    // querystring, so it must NOT be cached immutably or new builds never
    // reach the browser (it pulls the fresh ?v=<sha1> asset URLs). Fingerprinted
    // assets keep the long immutable TTL.
    const size_t rl = strlen(relpath);
    const bool isHtml = rl >= 5 && strcmp(relpath + rl - 5, ".html") == 0;
    httpd_resp_set_hdr(req, "Cache-Control",
                       isHtml ? "no-cache" : "max-age=31536000, immutable");
    if (gzipped) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    static constexpr size_t CHUNK = 16384;
    char* buf = (char*)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, CHUNK, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }
    fclose(f);
    heap_caps_free(buf);
    httpd_resp_send_chunk(req, nullptr, 0);
    return err;
}

static esp_err_t rootGetHandler(httpd_req_t* req) {
    return serveFile(req, "index.html");
}

// Catch-all: serve /<path> from LittleFS.  Skips paths that contain ".." so
// callers cannot escape the partition root.
static esp_err_t staticGetHandler(httpd_req_t* req) {
    const char* uri = req->uri;
    if (!uri || uri[0] != '/') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Bad URI", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    // Strip query string.
    char rel[128];
    const char* q = strchr(uri, '?');
    size_t len = q ? (size_t)(q - uri - 1) : strlen(uri + 1);
    if (len >= sizeof(rel)) len = sizeof(rel) - 1;
    memcpy(rel, uri + 1, len);
    rel[len] = '\0';

    if (rel[0] == '\0') return rootGetHandler(req);
    if (strstr(rel, "..")) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Bad URI", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    return serveFile(req, rel);
}

// ── WebSocket ────────────────────────────────────────────────────────────────

// Authoritative count of WS clients straight from esp_http_server.  We use
// this to reconcile wsClientCount whenever it appears to have drifted —
// close_fn occasionally misses decrements (e.g. when httpd_ws self-closes
// a socket whose FD has become invalid, it does not always invoke our
// close_fn), and after long-idle tunnel WS reconnects the counter can
// stay biased high and start rejecting legitimate browsers.  Returns -1
// on error.
// Iterate both servers (asset :80 carries LAN-direct WS clients; :81 carries
// the tunneled WS) and count WebSocket fds.  Returns -1 only if neither
// server is running.
static int wsCountFromHttpd() {
    if (!httpServer && !wsServer) return -1;
    int n = 0;
    struct { httpd_handle_t h; size_t max; } srv[] = {
        { httpServer, MAX_OPEN_SOCKETS_HTTP },
        { wsServer,   MAX_OPEN_SOCKETS_WS   },
    };
    int fds[MAX_OPEN_SOCKETS_HTTP];   // sized for the larger pool
    for (auto& s : srv) {
        if (!s.h) continue;
        size_t fds_count = s.max;
        if (httpd_get_client_list(s.h, &fds_count, fds) != ESP_OK) continue;
        for (size_t i = 0; i < fds_count; i++) {
            if (httpd_ws_get_fd_info(s.h, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) n++;
        }
    }
    return n;
}

// Post-handshake callback: invoked once when esp_http_server has finished
// the WebSocket upgrade.  This is the right place to count clients — the
// URI handler below is invoked only for frame data, never for the upgrade
// leg (see httpd_uri.c:362 in IDF: "If the request is websocket handshake,
// then do not call the uri->handler").
static esp_err_t wsPostHandshake(httpd_req_t* req) {
    int prev = wsClientCount.fetch_add(1, std::memory_order_acq_rel);
    if (prev >= WS_MAX_CLIENTS) {
        // Counter says cap hit — but it can drift upward when close_fn
        // misses decrements (tunnel WS reconnects, abrupt socket death).
        // Reconcile against httpd's real client list before rejecting.
        int actual = wsCountFromHttpd();
        if (actual >= 0 && actual <= WS_MAX_CLIENTS) {
            wsClientCount.store(actual, std::memory_order_release);
            ESP_LOGW(TAG, "wsClientCount drift corrected: %d → %d (allowing)",
                     prev + 1, actual);
        } else {
            wsClientCount.fetch_sub(1, std::memory_order_release);
            ESP_LOGW(TAG, "WS reject: %u clients already connected (actual=%d)",
                     (unsigned)prev, actual);
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "WS open  (clients=%d, fd=%d)",
             wsClientCount.load(), httpd_req_to_sockfd(req));
    return ESP_OK;
}

// Handle a single incoming WS frame.  esp_http_server only invokes this
// for frame data (the upgrade handshake is handled internally before the
// post-handshake callback fires).
static esp_err_t wsHandler(httpd_req_t* req) {
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;

    // First call with payload=null + len=0 returns the incoming length.
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws_recv_frame len: %s", esp_err_to_name(err));
        return err;
    }
    if (frame.len == 0) return ESP_OK;
    if (frame.len >= WS_QUEUE_MSG) {
        ESP_LOGW(TAG, "ws frame too long (%u)", (unsigned)frame.len);
        return ESP_OK;
    }

    uint8_t buf[WS_QUEUE_MSG];
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws_recv_frame data: %s", esp_err_to_name(err));
        return err;
    }
    buf[frame.len] = '\0';

    if (frame.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;

    const char* msg = (const char*)buf;
    if (strcmp(msg, "settings") == 0) {
        // Let the app push its snapshot into the queue, then drain so the
        // browser sees the initial state before the next main-loop tick.
        if (s_connCb) s_connCb();
        wsQueueDrain();
        return ESP_OK;
    }

    cJSON* root = cJSON_Parse(msg);
    if (!root) {
        ESP_LOGW(TAG, "ws json parse failed: %s", msg);
        return ESP_OK;
    }
    const cJSON* jid = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON* jvl = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (cJSON_IsString(jid) && cJSON_IsString(jvl) && s_cmdCb) {
        s_cmdCb(jid->valuestring, jvl->valuestring);
    } else {
        ESP_LOGW(TAG, "ws missing id/value: %s", msg);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// Track disconnects via the httpd close_fn hook so wsClientCount stays
// accurate.  esp_http_server invokes this for every socket close — both
// plain HTTP and WebSocket — so we discriminate by querying the session
// type.  Signature: void(*)(httpd_handle_t, int).
static void onHttpdClose(httpd_handle_t hd, int sockfd) {
    if (httpd_ws_get_fd_info(hd, sockfd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        if (wsClientCount.load() > 0) {
            wsClientCount.fetch_sub(1, std::memory_order_release);
        }
        ESP_LOGI(TAG, "WS close (clients=%d)", wsClientCount.load());
    }
    // Default close behaviour: shutdown + close the fd.
    shutdown(sockfd, SHUT_RDWR);
    close(sockfd);
}

// ── Broadcast queue ──────────────────────────────────────────────────────────

bool wsQueueSend(const char* msg) {
    if (!wsQueue || !msg) return false;
    size_t len = strlen(msg);
    if (len >= WS_QUEUE_MSG) len = WS_QUEUE_MSG - 1;
    char buf[WS_QUEUE_MSG];
    memcpy(buf, msg, len);
    buf[len] = '\0';
    return xQueueSend(wsQueue, buf, 0) == pdTRUE;
}

void wsQueueDrain() {
    if (!wsQueue || (!httpServer && !wsServer)) return;
    if (wsClientCount.load() == 0) {
        // Fast path: discard pending messages — no client to receive them.
        char tmp[WS_QUEUE_MSG];
        while (xQueueReceive(wsQueue, tmp, 0) == pdTRUE) { /* drop */ }
        return;
    }

    struct { httpd_handle_t h; size_t max; } srv[] = {
        { httpServer, MAX_OPEN_SOCKETS_HTTP },
        { wsServer,   MAX_OPEN_SOCKETS_WS   },
    };
    char buf[WS_QUEUE_MSG];
    while (xQueueReceive(wsQueue, buf, 0) == pdTRUE) {
        httpd_ws_frame_t frame = {};
        frame.type    = HTTPD_WS_TYPE_TEXT;
        frame.payload = (uint8_t*)buf;
        frame.len     = strlen(buf);

        int fds[MAX_OPEN_SOCKETS_HTTP];   // sized for the larger pool
        for (auto& s : srv) {
            if (!s.h) continue;
            size_t fds_count = s.max;
            if (httpd_get_client_list(s.h, &fds_count, fds) != ESP_OK) continue;
            for (size_t i = 0; i < fds_count; i++) {
                if (httpd_ws_get_fd_info(s.h, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
                esp_err_t e = httpd_ws_send_frame_async(s.h, fds[i], &frame);
                if (e != ESP_OK) {
                    ESP_LOGW(TAG, "ws_send fd=%d failed (%s), closing",
                             fds[i], esp_err_to_name(e));
                    httpd_sess_trigger_close(s.h, fds[i]);
                }
            }
        }
    }
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

esp_err_t startWebServer(WsCommandCb cb, WsConnectedCb conn) {
    if (httpServer) return ESP_OK;

    s_cmdCb  = cb;
    s_connCb = conn;

    if (!wsQueue) {
        // Storage is WS_QUEUE_LEN * WS_QUEUE_MSG = 12 KB. Place it in PSRAM
        // (xQueueCreateWithCaps) to spare scarce internal DRAM — the queue is
        // only touched from tasks (wsQueueSend/Drain), never from an ISR, so a
        // cache-backed PSRAM queue is safe here.
        wsQueue = xQueueCreateWithCaps(WS_QUEUE_LEN, WS_QUEUE_MSG, MALLOC_CAP_SPIRAM);
        if (!wsQueue) {
            ESP_LOGE(TAG, "wsQueue alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }

    // ── Asset server (port 80) ───────────────────────────────────────────
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_open_sockets = MAX_OPEN_SOCKETS_HTTP;
    cfg.max_uri_handlers = 4;
    cfg.stack_size       = 8192;
    cfg.lru_purge_enable = true;   // safe here: there are no long-lived sockets to evict
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.close_fn         = onHttpdClose;

    esp_err_t err = httpd_start(&httpServer, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start(:80) failed: %s", esp_err_to_name(err));
        httpServer = nullptr;
        return err;
    }

    // /ws is also registered on :80 so LAN/direct browsers (which hit
    // `ws://<esp>/ws`) keep working.  In LAN mode there is no tunnel-driven
    // asset stampede, so the LRU risk on :80 is acceptable.  Must be
    // registered BEFORE the "/*" wildcard, otherwise the wildcard match
    // already covers "/ws" and httpd refuses the registration as a dup.
    httpd_uri_t ws_uri_lan = {};
    ws_uri_lan.uri                      = "/ws";
    ws_uri_lan.method                   = HTTP_GET;
    ws_uri_lan.handler                  = wsHandler;
    ws_uri_lan.is_websocket             = true;
    ws_uri_lan.handle_ws_control_frames = false;
    ws_uri_lan.ws_post_handshake_cb     = wsPostHandshake;
    httpd_register_uri_handler(httpServer, &ws_uri_lan);

    httpd_uri_t root_uri = {};
    root_uri.uri     = "/";
    root_uri.method  = HTTP_GET;
    root_uri.handler = rootGetHandler;
    httpd_register_uri_handler(httpServer, &root_uri);

    httpd_uri_t any_uri = {};
    any_uri.uri     = "/*";
    any_uri.method  = HTTP_GET;
    any_uri.handler = staticGetHandler;
    httpd_register_uri_handler(httpServer, &any_uri);

    // ── WS server (port 81, reachable only via wstunnel routing) ─────────
    // Isolated socket pool so a burst of asset GETs through the reverse
    // tunnel cannot LRU-evict an open /ws.
    httpd_config_t wsCfg = HTTPD_DEFAULT_CONFIG();
    wsCfg.server_port      = 81;
    wsCfg.ctrl_port        = wsCfg.ctrl_port + 1;   // must differ from cfg.ctrl_port
    wsCfg.max_open_sockets = MAX_OPEN_SOCKETS_WS;
    wsCfg.max_uri_handlers = 2;
    wsCfg.stack_size       = 4096;   // WS server only handles upgrades; default 4 KB is enough
    wsCfg.lru_purge_enable = false;  // never evict a connected /ws; cap via wsPostHandshake
    wsCfg.uri_match_fn     = httpd_uri_match_wildcard;
    wsCfg.close_fn         = onHttpdClose;

    err = httpd_start(&wsServer, &wsCfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start(:81) failed: %s", esp_err_to_name(err));
        httpd_stop(httpServer);
        httpServer = nullptr;
        wsServer   = nullptr;
        return err;
    }

    httpd_uri_t ws_uri = {};
    ws_uri.uri                      = "/ws";
    ws_uri.method                   = HTTP_GET;
    ws_uri.handler                  = wsHandler;
    ws_uri.is_websocket             = true;
    ws_uri.handle_ws_control_frames = false;
    ws_uri.ws_post_handshake_cb     = wsPostHandshake;
    httpd_register_uri_handler(wsServer, &ws_uri);

    // Periodic background work for the WS pool:
    //   • Every 20 s: send a WebSocket PING (opcode 0x9, zero payload) to
    //     every connected WS fd.  Browsers reply with PONG automatically,
    //     so this stays out of the JS message stream.  The bytes flowing
    //     upstream keep nginx's `proxy_read_timeout` (default 60 s on Plesk)
    //     from killing the proxied WS when nothing else has changed on the
    //     firmware side for a while (e.g. heating off, BMS quiet).
    //   • Every 60 s: reconcile wsClientCount against the actual httpd
    //     client list.  close_fn occasionally misses decrements after
    //     abrupt socket teardowns, and a drifted counter biases the
    //     broadcast fast-path (which short-circuits on count==0).
    xTaskCreate([](void*) {
        int tick = 0;
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(20000));
            tick++;

            // Keepalive ping across both servers.  Each httpd serializes
            // sends per-fd internally, so this is safe to call without
            // extra locking; failures just log and let close_fn clean up.
            struct { httpd_handle_t h; size_t max; } srv[] = {
                { httpServer, MAX_OPEN_SOCKETS_HTTP },
                { wsServer,   MAX_OPEN_SOCKETS_WS   },
            };
            int fds[MAX_OPEN_SOCKETS_HTTP];
            httpd_ws_frame_t ping = {};
            ping.type    = HTTPD_WS_TYPE_PING;
            ping.payload = nullptr;
            ping.len     = 0;
            int sent = 0, failed = 0;
            for (auto& s : srv) {
                if (!s.h) continue;
                size_t fds_count = s.max;
                if (httpd_get_client_list(s.h, &fds_count, fds) != ESP_OK) continue;
                for (size_t i = 0; i < fds_count; i++) {
                    if (httpd_ws_get_fd_info(s.h, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
                    esp_err_t e = httpd_ws_send_frame_async(s.h, fds[i], &ping);
                    if (e == ESP_OK) {
                        sent++;
                    } else {
                        failed++;
                        ESP_LOGW(TAG, "ws ping fd=%d failed (%s), closing",
                                 fds[i], esp_err_to_name(e));
                        httpd_sess_trigger_close(s.h, fds[i]);
                    }
                }
            }
            if (sent || failed) ESP_LOGI(TAG, "ws keepalive ping: sent=%d failed=%d", sent, failed);

            // Counter sweep every third tick (~60 s).
            if (tick % 3 == 0) {
                int actual = wsCountFromHttpd();
                if (actual < 0) continue;
                int prev = wsClientCount.exchange(actual, std::memory_order_acq_rel);
                if (prev != actual) {
                    ESP_LOGW(TAG, "wsCount reaper: %d → %d", prev, actual);
                }
            }
        }
    }, "wsReaper", 3072, nullptr, 2, nullptr);

    ESP_LOGI(TAG, "web server up: :80 assets+WS, :81 WS-only (tunnel-routed)");
    return ESP_OK;
}

void stopWebServer() {
    if (wsServer) {
        httpd_stop(wsServer);
        wsServer = nullptr;
    }
    if (httpServer) {
        httpd_stop(httpServer);
        httpServer = nullptr;
    }
    s_cmdCb  = nullptr;
    s_connCb = nullptr;
}

#endif // WEBSERVER
