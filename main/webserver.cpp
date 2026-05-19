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

// ── Globals ──────────────────────────────────────────────────────────────────

httpd_handle_t   httpServer    = nullptr;
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
static constexpr uint8_t WS_MAX_CLIENTS = 4;

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

// Serve a file from /littlefs/<rel>, streaming in 1 KB chunks to keep peak
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
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=31536000, immutable");
    if (gzipped) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    // Stream in fixed-size chunks (smaller than IDF default 16 KB scratch).
    static constexpr size_t CHUNK = 1024;
    char buf[CHUNK];
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, nullptr, 0);     // end of stream
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

// Handle a single incoming WS frame.  esp_http_server invokes this same URI
// handler for the upgrade handshake (req->method == HTTP_GET) and for every
// subsequent frame (req->method == HTTP_GET still, but is_websocket already
// negotiated); we differentiate by examining ws_pkt.type.
static esp_err_t wsHandler(httpd_req_t* req) {
    // Handshake leg: no frame to read yet.
    if (req->method == HTTP_GET) {
        int prev = wsClientCount.fetch_add(1, std::memory_order_acq_rel);
        if (prev >= WS_MAX_CLIENTS) {
            wsClientCount.fetch_sub(1, std::memory_order_release);
            ESP_LOGW(TAG, "WS reject: %u clients already connected", (unsigned)prev);
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_send(req, "busy", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "WS handshake (clients=%d)", wsClientCount.load());
        return ESP_OK;
    }

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
    if (strcmp(msg, "ping") == 0) {
        if (s_cmdCb) s_cmdCb("/ping", "1");
        return ESP_OK;
    }
    if (strcmp(msg, "settings") == 0) {
        // Drain any queued frames first so we don't fight with them for the
        // per-client send window.
        wsQueueDrain();
        if (s_connCb) s_connCb();
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
    if (!wsQueue || !httpServer) return;
    if (wsClientCount.load() == 0) {
        // Fast path: discard pending messages — no client to receive them.
        char tmp[WS_QUEUE_MSG];
        while (xQueueReceive(wsQueue, tmp, 0) == pdTRUE) { /* drop */ }
        return;
    }

    char buf[WS_QUEUE_MSG];
    while (xQueueReceive(wsQueue, buf, 0) == pdTRUE) {
        size_t fds_count = WS_MAX_CLIENTS;
        int fds[WS_MAX_CLIENTS];
        if (httpd_get_client_list(httpServer, &fds_count, fds) != ESP_OK) continue;

        httpd_ws_frame_t frame = {};
        frame.type    = HTTPD_WS_TYPE_TEXT;
        frame.payload = (uint8_t*)buf;
        frame.len     = strlen(buf);
        for (size_t i = 0; i < fds_count; i++) {
            // Skip non-WebSocket sessions (plain HTTP sockets are also listed).
            if (httpd_ws_get_fd_info(httpServer, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
                continue;
            }
            esp_err_t e = httpd_ws_send_frame_async(httpServer, fds[i], &frame);
            if (e != ESP_OK) {
                ESP_LOGD(TAG, "ws_send fd=%d err=%s", fds[i], esp_err_to_name(e));
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
        wsQueue = xQueueCreate(WS_QUEUE_LEN, WS_QUEUE_MSG);
        if (!wsQueue) {
            ESP_LOGE(TAG, "wsQueue alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_open_sockets = 7;       // WS clients (4) + parallel asset GETs.
    cfg.max_uri_handlers = 8;
    cfg.stack_size       = 8192;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.close_fn         = onHttpdClose;

    esp_err_t err = httpd_start(&httpServer, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        httpServer = nullptr;
        return err;
    }

    // WebSocket endpoint first so it matches before the wildcard static
    // handler (LIFO would also work, but explicit order is clearer).
    httpd_uri_t ws_uri = {};
    ws_uri.uri               = "/ws";
    ws_uri.method            = HTTP_GET;
    ws_uri.handler           = wsHandler;
    ws_uri.is_websocket      = true;
    ws_uri.handle_ws_control_frames = false;
    httpd_register_uri_handler(httpServer, &ws_uri);

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

    ESP_LOGI(TAG, "web server up on :80 (LittleFS-backed)");
    return ESP_OK;
}

void stopWebServer() {
    if (!httpServer) return;
    httpd_stop(httpServer);
    httpServer = nullptr;
    s_cmdCb  = nullptr;
    s_connCb = nullptr;
}

#endif // WEBSERVER
