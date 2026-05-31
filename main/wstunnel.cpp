// WebSocket reverse-tunnel client — see wstunnel.hpp for the contract.
//
// Architecture
// ────────────
// One esp_websocket_client instance holds the control channel.  Its event
// handler runs on the websocket task and is responsible for accepting "open"
// and "data" / "close" frames coming from the server.  A dedicated "wstpump"
// task select()s on every active local TCP socket and ships outgoing bytes
// back to the server as JSON+base64 "data" frames.
//
// Reconnection is handled by esp_websocket_client itself (auto-reconnect
// with built-in exponential backoff).  We just (re)spawn the client when
// the user enables/disables the tunnel or changes its URL.

#include "wstunnel.hpp"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/tcp.h"   // TCP_NODELAY
#include "cJSON.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char* TAG = "wstunnel";

// Loopback ports of the two local httpd instances (see `webserver.cpp`).
//   :80 — static-asset server, LRU evicts idle keep-alives under load.
//   :81 — WS-only server, isolated pool so an asset stampede over the
//         reverse tunnel cannot drop an open /ws.
// `pick_local_port()` peeks the first request bytes to choose.
static constexpr uint16_t LOCAL_HTTP_PORT = 80;
static constexpr uint16_t LOCAL_WS_PORT   = 81;
static constexpr int      MAX_STREAMS     = 12;
// 4 KB per TLS record: halves the number of records for the typical
// 50–100 KB font transfer, ~25-40 % throughput win over 2 KB.  Safe now
// that PSRAM holds the mid-sized mbedtls allocations (sdkconfig threshold
// lowered to 2 KB) and our own scratch (`s_send_frame`, `s_pump_buf`,
// pending data, RX reassembly) is also out of internal heap, so the
// AES-GCM DMA buffer the HW crypto driver still needs in SRAM has room
// even with 4–5 simultaneous stream flushes during page-load bursts.
static constexpr size_t   IO_CHUNK        = 4096;
static constexpr size_t   RX_BUF_MAX      = 16 * 1024;   // cap on a single incoming frame
static constexpr size_t   PENDING_BUF_MAX = 16 * 1024;   // cap on per-stream pre-open buffer

struct Stream {
    bool     active;
    bool     is_ws;             // true if routed to LOCAL_WS_PORT — never evict
    uint32_t id;
    int      sock;              // non-blocking lwip socket to 127.0.0.1:80 or :81
    int64_t  last_active_us;    // last successful read/write, for idle eviction
};

// Idle keep-alive HTTP streams from previous page loads keep the browser's
// TCPs alive for ~5 min and so hold our slot table hostage.  When a new
// `open` would have nowhere to land, we evict the least-recently-active
// HTTP stream older than this grace period.  WS streams are exempt.
static constexpr int64_t IDLE_EVICT_US = 5000000;   // 5 s

static SemaphoreHandle_t      s_lock     = nullptr;     // protects cfg + streams
static EventGroupHandle_t     s_events   = nullptr;
static constexpr EventBits_t  EV_RECONNECT = BIT0;
static constexpr EventBits_t  EV_STOP      = BIT1;

static constexpr int FAIL_RETRY_LIMIT = 3;   // # of consecutive disconnects → FAILED

static constexpr int    PENDING_QUEUE_SIZE  = 16;   // max queued opens
static constexpr int64_t PENDING_TIMEOUT_US = 5000000;  // 5 s

static TunnelConfig                s_cfg = {};
static esp_websocket_client_handle_t s_client = nullptr;
static volatile bool               s_connected = false;
static volatile bool               s_have_ip   = false;   // STA has a usable IP
static volatile TunnelUiState      s_ui_state  = TunnelUiState::DISABLED;
static volatile int                s_retries   = 0;
static Stream                      s_streams[MAX_STREAMS] = {};

// Browsers fetch many assets in parallel (CSS/JS/fonts/etc), so the
// server may push more `open` frames than we can serve simultaneously
// without exhausting LWIP/httpd sockets.  Defer the overflow here and
// retry from the pump task as slots free up.  The Plesk bridge holds
// the corresponding browser TCP sockets open, so the browser sees a
// slight latency instead of a 502.
//
// `data` is the per-id buffer for bytes that arrive on the WS while the
// open is still queued.  Without it the server's first request bytes
// (the browser's GET line + headers) would be dropped, and even when
// the stream eventually materialises the httpd would just sit waiting
// for a request that never arrives.
struct Pending {
    uint32_t id;
    int64_t  enq_us;
    uint8_t* data;
    size_t   data_len;
    size_t   data_cap;
};
static Pending s_pending[PENDING_QUEUE_SIZE] = {};
static int     s_pending_n = 0;

static void pending_free_locked(int i) {
    free(s_pending[i].data);
    s_pending[i].data     = nullptr;
    s_pending[i].data_len = 0;
    s_pending[i].data_cap = 0;
}

static void pending_remove_locked(int i) {
    pending_free_locked(i);
    s_pending[i] = s_pending[--s_pending_n];
}

// Re-assembly buffer for fragmented incoming WS frames.  s_rx_op_code
// holds the opcode of the in-progress frame so continuation events (which
// carry op_code 0) get routed to the right branch on completion.
static char*  s_rx          = nullptr;
static size_t s_rx_cap      = 0;
static size_t s_rx_len      = 0;
static int    s_rx_op_code  = 0;

// ── NVS helpers ───────────────────────────────────────────────────────────────

static void nvs_get_str_kv(const char* ns, const char* key, char* buf, size_t cap)
{
    buf[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_str(h, key, buf, &cap);
    nvs_close(h);
}

static void nvs_set_str_kv(const char* ns, const char* key, const char* val)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

static uint8_t nvs_get_u8_kv(const char* ns, const char* key, uint8_t def)
{
    uint8_t v = def;
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return def;
    nvs_get_u8(h, key, &v);
    nvs_close(h);
    return v;
}

static void nvs_set_u8_kv(const char* ns, const char* key, uint8_t val)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

// ── Stream table helpers (caller holds s_lock) ────────────────────────────────

static Stream* find_stream_locked(uint32_t id)
{
    for (auto& s : s_streams) if (s.active && s.id == id) return &s;
    return nullptr;
}

static Stream* alloc_stream_locked(uint32_t id, int sock, bool is_ws)
{
    for (auto& s : s_streams) {
        if (!s.active) {
            s.active         = true;
            s.is_ws          = is_ws;
            s.id             = id;
            s.sock           = sock;
            s.last_active_us = esp_timer_get_time();
            return &s;
        }
    }
    return nullptr;
}

static void free_stream_locked(Stream& s)
{
    if (!s.active) return;
    if (s.sock >= 0) lwip_close(s.sock);
    s.active         = false;
    s.is_ws          = false;
    s.id             = 0;
    s.sock           = -1;
    s.last_active_us = 0;
}

static bool send_ctrl(const char* type, uint32_t id);   // forward decl

// When the slot table is full, reclaim one idle HTTP stream (NEVER a WS) so
// a fresh open can land instead of timing out in the pending queue.  Picks
// the least-recently-active HTTP stream older than IDLE_EVICT_US.  Returns
// true if a slot was freed.  Sends a `close` to the server so the bridge
// can synthesise a 502 to the browser, which will reopen on its next nav.
static bool evict_idle_locked()
{
    int64_t now = esp_timer_get_time();
    Stream* victim  = nullptr;
    int64_t  oldest = now;
    for (auto& s : s_streams) {
        if (!s.active || s.is_ws) continue;
        if (now - s.last_active_us < IDLE_EVICT_US) continue;
        if (s.last_active_us < oldest) { oldest = s.last_active_us; victim = &s; }
    }
    if (!victim) return false;
    uint32_t vid = victim->id;
    ESP_LOGI(TAG, "evicting idle stream %u (idle %lld ms)",
             (unsigned)vid, (now - victim->last_active_us) / 1000);
    free_stream_locked(*victim);
    send_ctrl("close", vid);
    return true;
}

static void drop_all_streams_locked()
{
    for (auto& s : s_streams) free_stream_locked(s);
}

// ── Local loopback socket ─────────────────────────────────────────────────────

static int connect_local(uint16_t port)
{
    int s = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in sa = {};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = htons(port);
    if (lwip_connect(s, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        lwip_close(s); return -1;
    }
    int flags = lwip_fcntl(s, F_GETFL, 0);
    lwip_fcntl(s, F_SETFL, flags | O_NONBLOCK);
    // Loopback has zero RTT — Nagle's coalescing buys nothing and actively
    // hurts:  a 50-byte LCD-originated `setting` frame from httpd:81 or a
    // 2-byte PING/PONG sits in the kernel TX buffer for the delayed-ACK
    // timer (up to 200 ms × N retries) waiting for "more data" before
    // flushing.  Through the tunnel that propagates as multi-second
    // perceived latency on web UI updates.  Disable on our side; the httpd
    // side flushes per write anyway because chunks are emitted individually.
    int yes = 1;
    lwip_setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return s;
}

// Choose the local httpd port for a tunneled stream from its first bytes.
// Returns 0 if the caller must wait for more data before deciding (we need
// at least the request line, i.e. `GET /<path> `).  HTTP request methods
// are always uppercase ASCII so a literal memcmp is fine.
static uint16_t pick_local_port(const uint8_t* data, size_t len)
{
    // Smallest discriminator: "GET /ws " (8 bytes).  Anything shorter and
    // we can't tell yet — caller defers.
    if (!data || len < 8) return 0;
    if (memcmp(data, "GET /ws", 7) == 0) {
        char c = (char)data[7];
        if (c == ' ' || c == '?' || c == '/') return LOCAL_WS_PORT;
    }
    return LOCAL_HTTP_PORT;
}

// ── Outbound: send a control / data frame to the server ───────────────────────

// Returns false if the send was dropped — caller should treat the stream
// as dead and tell the server to close it.
//
// Control frames (close, hello) are tiny (~26 B) and use a short timeout
// (1 s): if TLS can't flush 26 bytes in 1 s the connection is stalled
// and blocking longer just cascades into other streams (the pump task
// is single-threaded).  Data frames are larger and tolerate more TLS
// backpressure (5 s), but still cap at half the old 10 s to limit how
// long the pump loop freezes when the remote is slow.
static constexpr TickType_t SEND_CTRL_TICKS = pdMS_TO_TICKS(1000);
static constexpr TickType_t SEND_DATA_TICKS = pdMS_TO_TICKS(5000);

static bool send_text(const char* json, TickType_t timeout = SEND_CTRL_TICKS)
{
    if (!s_client || !esp_websocket_client_is_connected(s_client)) return false;
    int n = esp_websocket_client_send_text(s_client, json, strlen(json), timeout);
    if (n < 0) {
        ESP_LOGW(TAG, "ws send dropped (n=%d, len=%u)", n, (unsigned)strlen(json));
        return false;
    }
    return true;
}

static bool send_ctrl(const char* type, uint32_t id)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"id\":%u}", type, (unsigned)id);
    return send_text(buf, SEND_CTRL_TICKS);
}

// Binary WS frame: 4-byte BE stream id || payload bytes.  Skips base64
// (saves ~33% bandwidth) and JSON wrap (saves a parse/format).  hello /
// open / close stay JSON over text frames; only the bulk data path
// goes binary.
// Scratch buffer for the outgoing binary frame.  Lives in PSRAM (allocated
// once in wstunnelInit) so the ~2 KB do not eat into internal DRAM that
// esp-aes / PSA need for their DMA-capable allocations during TLS.  Safe
// because esp_websocket_client_send_bin copies into its TLS output buffer
// before encrypting — input does not need to be DMA-capable.
static uint8_t* s_send_frame = nullptr;

static bool send_data(uint32_t id, const uint8_t* bytes, size_t len)
{
    if (!s_client || !esp_websocket_client_is_connected(s_client)) return false;
    if (!s_send_frame) return false;
    if (len > IO_CHUNK) return false;   // caller's contract: pump_task reads IO_CHUNK max
    s_send_frame[0] = (uint8_t)(id >> 24);
    s_send_frame[1] = (uint8_t)(id >> 16);
    s_send_frame[2] = (uint8_t)(id >>  8);
    s_send_frame[3] = (uint8_t)(id      );
    memcpy(s_send_frame + 4, bytes, len);
    int n = esp_websocket_client_send_bin(s_client, (const char*)s_send_frame, 4 + len,
                                          SEND_DATA_TICKS);
    if (n < 0) {
        ESP_LOGW(TAG, "ws send_bin dropped (n=%d, len=%u)", n, (unsigned)(4 + len));
        return false;
    }
    return true;
}

// ── Inbound frame handling ────────────────────────────────────────────────────

// Try to materialise an `open` frame into a live stream.  If `prebuf`
// is non-null it carries bytes the server already pushed while the open
// was queued; we flush them to the local socket before returning.  Returns
// true if the stream is now active; false if the caller should keep
// trying later (slot still unavailable, or local write failed).
// Caller holds s_lock.
static bool try_open_locked(uint32_t id,
                            const uint8_t* prebuf = nullptr,
                            size_t prebuf_len = 0)
{
    // Need enough of the request line to pick :80 vs :81.  Bail without
    // touching slots so the caller keeps the pending entry and retries on
    // the next pump tick once more bytes arrive (or the 5 s timeout fires).
    uint16_t port = pick_local_port(prebuf, prebuf_len);
    if (port == 0) return false;
    // Debug: log the request-line prefix so we can verify the sniff sees
    // what we expect (e.g. relative vs absolute-form, methods other than
    // GET).  Remove once the routing is confirmed correct.
    if (prebuf_len > 0) {
        size_t n = prebuf_len > 40 ? 40 : prebuf_len;
        ESP_LOGI(TAG, "stream %u sniff (port=%u): \"%.*s\"",
                 (unsigned)id, port, (int)n, (const char*)prebuf);
    }

    bool any_free = false;
    for (auto& sl : s_streams) if (!sl.active) { any_free = true; break; }
    if (!any_free) {
        // Slot table full of zombie keep-alive HTTPs.  Reclaim one before
        // refusing.  If no candidate is idle enough, the pending entry
        // sticks around (and the page-load grace expires it after 5 s).
        if (!evict_idle_locked()) return false;
    }

    int sock = connect_local(port);
    if (sock < 0) return false;        // LWIP exhausted / httpd refused

    Stream* s = alloc_stream_locked(id, sock, port == LOCAL_WS_PORT);
    if (!s) {                          // raced with another open; bail
        lwip_close(sock);
        return false;
    }

    // Flush the buffered request bytes — at this point `prebuf` is always
    // non-empty because pick_local_port() refused to choose otherwise.
    size_t off = 0;
    int    spins = 0;
    while (off < prebuf_len) {
        ssize_t w = lwip_write(sock, prebuf + off, prebuf_len - off);
        if (w > 0) { off += w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && spins++ < 50) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        ESP_LOGW(TAG, "stream %u: flush of %u prebuf bytes failed at %u (errno=%d)",
                 (unsigned)id, (unsigned)prebuf_len, (unsigned)off, errno);
        free_stream_locked(*s);
        return false;
    }
    ESP_LOGI(TAG, "stream %u → :%u, flushed %u buffered bytes",
             (unsigned)id, (unsigned)port, (unsigned)prebuf_len);
    return true;
}

static void enqueue_pending_locked(uint32_t id)
{
    if (s_pending_n >= PENDING_QUEUE_SIZE) {
        ESP_LOGW(TAG, "stream %u: pending queue full, dropping", (unsigned)id);
        send_ctrl("close", id);
        return;
    }
    s_pending[s_pending_n].id       = id;
    s_pending[s_pending_n].enq_us   = esp_timer_get_time();
    s_pending[s_pending_n].data     = nullptr;
    s_pending[s_pending_n].data_len = 0;
    s_pending[s_pending_n].data_cap = 0;
    s_pending_n++;
    ESP_LOGI(TAG, "stream %u queued (pending=%d)", (unsigned)id, s_pending_n);
}

// Try every queued open; drop entries whose age exceeds PENDING_TIMEOUT_US.
// Caller holds s_lock.
static void drain_pending_locked()
{
    int64_t now = esp_timer_get_time();
    int i = 0;
    while (i < s_pending_n) {
        if (try_open_locked(s_pending[i].id,
                            s_pending[i].data, s_pending[i].data_len)) {
            pending_remove_locked(i);
            continue;
        }
        if (now - s_pending[i].enq_us > PENDING_TIMEOUT_US) {
            ESP_LOGW(TAG, "stream %u: pending timeout, closing",
                     (unsigned)s_pending[i].id);
            send_ctrl("close", s_pending[i].id);
            pending_remove_locked(i);
            continue;
        }
        i++;
    }
}

static void handle_open(uint32_t id)
{
    // Always defer: try_open_locked() needs the first request bytes to
    // decide between :80 (assets) and :81 (WS).  drain_pending_locked()
    // materialises the stream on the next pump tick once data arrives
    // (the server pushes the HTTP request line right after `open`, so
    // the latency is sub-frame in practice).
    xSemaphoreTake(s_lock, portMAX_DELAY);
    enqueue_pending_locked(id);
    xSemaphoreGive(s_lock);
}

// Bytes from the server destined for stream `id`'s local socket.  Called
// from the binary-frame branch of the WS event handler (we no longer
// receive data inside JSON).
static void handle_data(uint32_t id, const uint8_t* raw, size_t rlen)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    Stream* s = find_stream_locked(id);
    int sock  = s ? s->sock : -1;
    if (sock < 0) {
        // No active stream — check the pending queue: server might have
        // pushed the GET request bytes before we managed to allocate a
        // slot for the open frame.  Buffer them and replay on materialise.
        for (int i = 0; i < s_pending_n; i++) {
            if (s_pending[i].id != id) continue;
            Pending& p = s_pending[i];
            size_t need = p.data_len + rlen;
            if (need > PENDING_BUF_MAX) {
                ESP_LOGW(TAG, "stream %u: pending buffer overflow, closing",
                         (unsigned)id);
                send_ctrl("close", id);
                pending_remove_locked(i);
                xSemaphoreGive(s_lock);
                return;
            }
            if (p.data_cap < need) {
                // PSRAM-resident: spares internal DRAM for the TLS path.
                uint8_t* nb = (uint8_t*)heap_caps_realloc(p.data, need, MALLOC_CAP_SPIRAM);
                if (!nb) {
                    ESP_LOGW(TAG, "stream %u: pending realloc failed", (unsigned)id);
                    send_ctrl("close", id);
                    pending_remove_locked(i);
                    xSemaphoreGive(s_lock);
                    return;
                }
                p.data     = nb;
                p.data_cap = need;
            }
            memcpy(p.data + p.data_len, raw, rlen);
            p.data_len = need;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "stream %u: %u bytes buffered (pending=%u total)",
                     (unsigned)id, (unsigned)rlen, (unsigned)p.data_len);
            return;
        }
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "stream %u: data %u bytes dropped (no stream)",
                 (unsigned)id, (unsigned)rlen);
        return;
    }
    s->last_active_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "stream %u: %u bytes ← server", (unsigned)id, (unsigned)rlen);

    // Push everything; the loopback socket should always drain quickly.
    size_t off = 0;
    while (off < rlen) {
        ssize_t w = lwip_write(sock, raw + off, rlen - off);
        if (w > 0) { off += w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        // Local socket gone — tear the stream down.
        ESP_LOGI(TAG, "stream %u: local write failed (errno=%d)", (unsigned)id, errno);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        Stream* s2 = find_stream_locked(id);
        if (s2) free_stream_locked(*s2);
        xSemaphoreGive(s_lock);
        send_ctrl("close", id);
        break;
    }
}

static void handle_close(uint32_t id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    Stream* s = find_stream_locked(id);
    if (s) free_stream_locked(*s);
    // Also drop any matching entry from the pending queue — the browser
    // gave up before we could allocate sockets for it.
    for (int i = 0; i < s_pending_n; i++) {
        if (s_pending[i].id == id) {
            pending_remove_locked(i);
            break;
        }
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "stream %u closed by peer", (unsigned)id);
}

static void parse_and_dispatch(const char* json, size_t len)
{
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!root) return;
    cJSON* j_type = cJSON_GetObjectItem(root, "type");
    cJSON* j_id   = cJSON_GetObjectItem(root, "id");
    if (!cJSON_IsString(j_type) || !cJSON_IsNumber(j_id)) { cJSON_Delete(root); return; }
    uint32_t id = (uint32_t)j_id->valuedouble;
    const char* type = j_type->valuestring;
    if (strcmp(type, "open") == 0) {
        handle_open(id);
    } else if (strcmp(type, "close") == 0) {
        handle_close(id);
    }
    // "data" arrives as a binary WS frame now — handled directly in the
    // event callback, not here.
    cJSON_Delete(root);
}

// ── esp_websocket_client event handler ────────────────────────────────────────

static void send_hello()
{
    char token[sizeof(s_cfg.token)];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(token, s_cfg.token, sizeof(token));
    token[sizeof(token) - 1] = '\0';
    xSemaphoreGive(s_lock);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"hello\",\"node\":\"truminus\",\"token\":\"%s\"}", token);
    send_text(buf);
}

static void on_ws_event(void* /*arg*/, esp_event_base_t /*base*/,
                        int32_t event_id, void* event_data)
{
    auto* ev = (esp_websocket_event_data_t*)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to %s", s_cfg.server);
        s_connected = true;
        s_retries   = 0;
        s_ui_state  = TunnelUiState::CONNECTED;
        s_rx_len    = 0;
        send_hello();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "disconnected (attempt %d/%d)", (int)s_retries + 1, FAIL_RETRY_LIMIT);
        s_connected = false;
        s_rx_len    = 0;
        // Count consecutive failed (re)connection attempts; once we cross
        // the threshold the topbar icon turns red so the user can tell the
        // tunnel is mis-configured / server unreachable, rather than just
        // "still trying".  The websocket client keeps retrying in the
        // background regardless.
        s_retries = s_retries + 1;
        if (s_retries >= FAIL_RETRY_LIMIT) {
            s_ui_state = TunnelUiState::FAILED;
        } else if (s_ui_state != TunnelUiState::FAILED) {
            s_ui_state = TunnelUiState::CONNECTING;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        drop_all_streams_locked();
        while (s_pending_n > 0) pending_remove_locked(0);
        xSemaphoreGive(s_lock);
        break;
    case WEBSOCKET_EVENT_DATA: {
        // The IDF sometimes leaks the FIN bit (0x80) into op_code on top of
        // the WS opcode — mask it off so 0x82 (binary+FIN) still matches.
        int op = ev->op_code & 0x0F;
        // op 1 = text (control JSON), 2 = binary (stream payload),
        // 0 = continuation (carries the type of the frame it continues).
        // Ping/pong/close are ignored.
        if (op != 0x01 && op != 0x02 && op != 0x00) break;
        if (ev->data_len <= 0) break;

        // Re-assemble fragmented frames.  payload_len is the full frame size.
        size_t total = (size_t)ev->payload_len;
        if (total > RX_BUF_MAX) {
            ESP_LOGW(TAG, "frame too big: %u", (unsigned)total);
            s_rx_len = 0; break;
        }
        if (s_rx_cap < total + 1) {
            // PSRAM-resident: up to RX_BUF_MAX (16 KB) of reassembly buffer
            // out of internal DRAM.
            char* nb = (char*)heap_caps_realloc(s_rx, total + 1, MALLOC_CAP_SPIRAM);
            if (!nb) { s_rx_len = 0; break; }
            s_rx     = nb;
            s_rx_cap = total + 1;
        }
        size_t off = (size_t)ev->payload_offset;
        if (off == 0 && op != 0x00) {
            s_rx_op_code = op;                     // remember type for continuations
        }
        if (off + ev->data_len > s_rx_cap) break;
        memcpy(s_rx + off, ev->data_ptr, ev->data_len);
        s_rx_len = off + ev->data_len;
        if (s_rx_len >= total) {
            ESP_LOGD(TAG, "rx complete: op=0x%02x raw_op=0x%02x total=%u",
                     s_rx_op_code, ev->op_code, (unsigned)total);
            if (s_rx_op_code == 0x01) {
                s_rx[total] = '\0';
                parse_and_dispatch(s_rx, total);
            } else if (s_rx_op_code == 0x02 && total >= 4) {
                uint32_t id = ((uint32_t)(uint8_t)s_rx[0] << 24) |
                              ((uint32_t)(uint8_t)s_rx[1] << 16) |
                              ((uint32_t)(uint8_t)s_rx[2] <<  8) |
                              ((uint32_t)(uint8_t)s_rx[3]);
                handle_data(id, (const uint8_t*)s_rx + 4, total - 4);
            } else {
                ESP_LOGW(TAG, "rx dropped: s_rx_op_code=0x%02x raw_op=0x%02x total=%u",
                         s_rx_op_code, ev->op_code, (unsigned)total);
            }
            s_rx_len = 0;
        }
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "ws error");
        break;
    default: break;
    }
}

// ── Pump task: drain local sockets → WS data frames ───────────────────────────

static uint8_t* s_pump_buf = nullptr;   // IO_CHUNK bytes in PSRAM, see wstunnelInit

static void pump_task(void*)
{
    // Single owner (this task), allocated in PSRAM at init.  Keeps the
    // task stack small and frees IO_CHUNK bytes of internal DRAM.
    uint8_t* buf = s_pump_buf;
    for (;;) {
        if (!s_connected) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Drain the deferred-open queue first — a slot may have freed up
        // since the last iteration.  Cheap when the queue is empty.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_pending_n > 0) drain_pending_locked();
        // Snapshot the active stream list under the lock; do the I/O without
        // holding it so a slow lwip_read can't block the WS RX path.
        struct Snap { uint32_t id; int sock; };
        Snap snaps[MAX_STREAMS];
        int n = 0;
        for (auto& s : s_streams) if (s.active) snaps[n++] = { s.id, s.sock };
        xSemaphoreGive(s_lock);

        bool any = false;
        for (int i = 0; i < n; i++) {
            ssize_t r = lwip_read(snaps[i].sock, buf, IO_CHUNK);
            if (r > 0) {
                // Coalesce: drain whatever else lwip already has queued for
                // this fd so we don't ship the response in 4-byte slivers.
                // httpd's chunked-transfer encoding emits size-hex, payload
                // and CRLF as separate sends, and the loopback queue
                // surfaces them one-by-one.  Each TLS write has fixed
                // crypto + framing overhead, so batching turns a 100-frame
                // font transfer into ~5 frames.
                while (r < (ssize_t)IO_CHUNK) {
                    ssize_t r2 = lwip_read(snaps[i].sock, buf + r, IO_CHUNK - r);
                    if (r2 <= 0) break;          // EAGAIN, EOF or error
                    r += r2;
                }
                ESP_LOGI(TAG, "stream %u: %u bytes → server", (unsigned)snaps[i].id, (unsigned)r);
                // Bump last-active so the idle-eviction LRU doesn't reap
                // this stream mid-transfer.
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (Stream* s2 = find_stream_locked(snaps[i].id)) {
                    s2->last_active_us = esp_timer_get_time();
                }
                xSemaphoreGive(s_lock);
                // If the WS send fails the stream is doomed — tear it down
                // immediately so we don't half-flush gzip/HTTP content to
                // the browser (manifests as ERR_CONTENT_DECODING_FAILED).
                // Also break out of the loop: if TLS can't flush one frame,
                // trying more streams just piles up timeouts and starves
                // every loopback socket in the table.
                if (!send_data(snaps[i].id, buf, (size_t)r)) {
                    bool was_active = false;
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    Stream* s2 = find_stream_locked(snaps[i].id);
                    if (s2 && s2->sock == snaps[i].sock) {
                        free_stream_locked(*s2);
                        was_active = true;
                    }
                    xSemaphoreGive(s_lock);
                    if (was_active) send_ctrl("close", snaps[i].id);
                    break;
                }
                any = true;
            } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                // Local end closed or unreadable.  Distinguish three cases
                // under the lock:
                //  (a) stream still has our snapshotted fd → normal close
                //      path (EOF / ENOTCONN / write-side error).
                //  (b) stream gone or fd reassigned → another path
                //      (handle_close, handle_data write-fail) already
                //      tore it down and possibly recycled the fd; skip
                //      silently so we don't double-send a close, and
                //      certainly don't act on a stranger's fd.
                //  (c) EBADF specifically means our fd was closed
                //      already — never log loud; (a)/(b) below decide.
                bool already_gone = false;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                Stream* s2 = find_stream_locked(snaps[i].id);
                if (s2 && s2->sock == snaps[i].sock) {
                    free_stream_locked(*s2);
                } else {
                    already_gone = true;
                }
                xSemaphoreGive(s_lock);
                if (already_gone) continue;
                if (r == 0 || (r < 0 && (errno == ENOTCONN || errno == EBADF))) {
                    ESP_LOGD(TAG, "stream %u: local closed (r=%d errno=%d)",
                             (unsigned)snaps[i].id, (int)r, (int)errno);
                } else {
                    ESP_LOGW(TAG, "stream %u: local read errno=%d, closing",
                             (unsigned)snaps[i].id, errno);
                }
                send_ctrl("close", snaps[i].id);
            }
        }
        if (!any) vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ── Client lifecycle ──────────────────────────────────────────────────────────

// Tears the client down but does NOT touch s_ui_state — callers set the
// UI state explicitly so we can distinguish "user disabled it" (grey) from
// "WiFi dropped, still trying" (blink).
static void stop_client_locked()
{
    if (!s_client) return;
    esp_websocket_client_stop(s_client);
    esp_websocket_client_destroy(s_client);
    s_client    = nullptr;
    s_connected = false;
    s_retries   = 0;
    drop_all_streams_locked();
    while (s_pending_n > 0) pending_remove_locked(0);
}

static void start_client_locked()
{
    stop_client_locked();
    if (!s_cfg.enabled || !s_cfg.server[0]) {
        ESP_LOGI(TAG, "disabled or no server URL — not starting");
        return;
    }
    // Don't even try to spin up esp_websocket_client until the STA has an
    // IP — without one esp-tls fails getaddrinfo() (ESP_ERR_ESP_TLS_CANNOT
    // _RESOLVE_HOSTNAME) and the client just thrashes.  We re-enter here
    // from the IP_EVENT_STA_GOT_IP handler.
    //
    // Stay on DISABLED (grey icon) until WiFi is up: the user already has
    // a separate WiFi indicator on the topbar; we don't want a blinking
    // cloud confusing them while the real problem is "no network yet".
    if (!s_have_ip) {
        ESP_LOGI(TAG, "STA not connected yet — waiting for IP");
        s_ui_state = TunnelUiState::DISABLED;
        return;
    }
    // The settings screen only takes the bare hostname (e.g.
    // "truminus.victordorado.es"); we own the scheme, path and token query
    // param so the bridge contract stays inside the firmware.  We still
    // tolerate a leading wss:// / ws:// and any trailing path the user
    // typed (legacy NVS values, copy-pasted URLs) by stripping them.
    const char* host = s_cfg.server;
    if      (strncmp(host, "wss://", 6) == 0) host += 6;
    else if (strncmp(host, "ws://",  5) == 0) host += 5;
    char host_buf[sizeof(s_cfg.server)];
    strncpy(host_buf, host, sizeof(host_buf));
    host_buf[sizeof(host_buf) - 1] = '\0';
    if (char* cut = strpbrk(host_buf, "/?")) *cut = '\0';

    char url[sizeof(s_cfg.server) + sizeof(s_cfg.token) + 32];
    if (s_cfg.token[0]) {
        snprintf(url, sizeof(url), "wss://%s/tunnel?token=%s", host_buf, s_cfg.token);
    } else {
        snprintf(url, sizeof(url), "wss://%s/tunnel", host_buf);
    }

    esp_websocket_client_config_t cfg = {};
    cfg.uri                  = url;
    cfg.reconnect_timeout_ms = 5000;
    // 25 s tolerates Passenger cold-start congestion: when the bridge's
    // Node process is being spun up, the upstream WS socket can stall
    // writing for 10+ s.  The default 10 s timeout fires EAGAIN, our pump
    // tears the iteration down, and every in-flight asset GET returns
    // 502 to the browser.
    cfg.network_timeout_ms   = 25000;
    cfg.buffer_size          = 8192;   // ≥ IO_CHUNK + 4 (binary stream-id header) with headroom
    // Required for wss:// against a Let's Encrypt cert: without a trust
    // store esp-tls fails the handshake silently and the WS client just
    // keeps "reconnecting" forever.  The IDF certificate bundle covers
    // every public CA (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL).
    cfg.crt_bundle_attach    = esp_crt_bundle_attach;

    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) {
        ESP_LOGW(TAG, "init failed");
        s_ui_state = TunnelUiState::FAILED;
        return;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_ws_event, nullptr);
    esp_websocket_client_start(s_client);
    s_retries  = 0;
    s_ui_state = TunnelUiState::CONNECTING;
    ESP_LOGI(TAG, "started, connecting to %s", s_cfg.server);
}

static void supervisor_task(void*)
{
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(
            s_events, EV_RECONNECT | EV_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (bits & EV_STOP) {
            stop_client_locked();
            // If the tunnel is still enabled in config, the stop came from
            // WiFi loss (or a transient nudge) — keep blinking so the user
            // knows we're still trying.  Otherwise the user toggled it off.
            s_ui_state = s_cfg.enabled ? TunnelUiState::CONNECTING
                                       : TunnelUiState::DISABLED;
        }
        if (bits & EV_RECONNECT) start_client_locked();
        xSemaphoreGive(s_lock);
    }
}

// ── Network event handlers ────────────────────────────────────────────────────

static void on_net_event(void* /*arg*/, esp_event_base_t base,
                         int32_t id, void* /*data*/)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_have_ip = true;
        // Kick the supervisor so it re-enters start_client_locked now that
        // the network is usable.  If the tunnel is disabled this is a NOP.
        if (s_events && s_cfg.enabled) xEventGroupSetBits(s_events, EV_RECONNECT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_have_ip = false;
        // Tear the client down immediately — leaving it running while the
        // STA is down just produces the DNS-resolution thrash we just
        // fixed.  Don't reset the UI state to DISABLED: if the tunnel is
        // enabled we want the icon to keep blinking (CONNECTING) until
        // WiFi comes back.
        if (s_events) xEventGroupSetBits(s_events, EV_STOP);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void wstunnelLoadConfig(TunnelConfig& out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out = s_cfg;
    xSemaphoreGive(s_lock);
}

void wstunnelSaveConfig(const TunnelConfig& cfg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = cfg;
    s_cfg.server[sizeof(s_cfg.server) - 1] = '\0';
    s_cfg.token [sizeof(s_cfg.token)  - 1] = '\0';
    nvs_set_u8_kv ("tunnel", "enabled", s_cfg.enabled ? 1 : 0);
    nvs_set_str_kv("tunnel", "server",  s_cfg.server);
    nvs_set_str_kv("tunnel", "token",   s_cfg.token);
    xSemaphoreGive(s_lock);
}

void wstunnelApply()
{
    if (!s_events) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool en = s_cfg.enabled;
    xSemaphoreGive(s_lock);
    xEventGroupSetBits(s_events, en ? EV_RECONNECT : EV_STOP);
}

void wstunnelSuspend()
{
    if (!s_events) return;
    // EV_STOP destroys the client but keeps s_cfg.enabled, so the icon shows
    // CONNECTING (blinking) rather than DISABLED — we intend to come back.
    xEventGroupSetBits(s_events, EV_STOP);
}

bool wstunnelIsConnected() { return s_connected; }

TunnelUiState wstunnelUiState() { return s_ui_state; }

void wstunnelInit()
{
    static bool inited = false;
    if (inited) return;
    inited = true;

    s_lock   = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    for (auto& s : s_streams) { s.active = false; s.sock = -1; }

    // Long-lived I/O buffers in PSRAM.  Keeps ~6 KB of internal DRAM free
    // for esp-aes (DMA) and PSA crypto contexts that have to live in
    // SRAM.  Fail loudly if PSRAM isn't available — the tunnel would
    // crash on the first send_data() / read otherwise.
    s_send_frame = (uint8_t*)heap_caps_malloc(IO_CHUNK + 4, MALLOC_CAP_SPIRAM);
    s_pump_buf   = (uint8_t*)heap_caps_malloc(IO_CHUNK,     MALLOC_CAP_SPIRAM);
    if (!s_send_frame || !s_pump_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed (send=%p pump=%p)", s_send_frame, s_pump_buf);
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.enabled = nvs_get_u8_kv("tunnel", "enabled", 0) != 0;
    nvs_get_str_kv("tunnel", "server", s_cfg.server, sizeof(s_cfg.server));
    nvs_get_str_kv("tunnel", "token",  s_cfg.token,  sizeof(s_cfg.token));
    // Reflect the saved state on the icon before we have an IP — if the
    // user had it enabled, we should already be "connecting" (blinking).
    s_ui_state = s_cfg.enabled ? TunnelUiState::CONNECTING
                               : TunnelUiState::DISABLED;
    xSemaphoreGive(s_lock);

    // Subscribe to STA up/down so we (re)start exactly when DNS is usable.
    // If WiFi was already up by the time we get here (typical), check
    // immediately so we don't sit waiting for an event that already fired.
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,
                               &on_net_event, nullptr);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                               &on_net_event, nullptr);
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t ip = {};
        if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
            s_have_ip = true;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cfg.enabled) start_client_locked();
    xSemaphoreGive(s_lock);

    xTaskCreate(supervisor_task, "wstsup",  4096, nullptr, 4, nullptr);
    xTaskCreate(pump_task,       "wstpump", 6144, nullptr, 4, nullptr);
}
