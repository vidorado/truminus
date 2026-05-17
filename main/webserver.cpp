#include "webserver.hpp"
#include "logs.hpp"
#ifdef WEBSERVER
#include <WiFi.h>

// Static web files are embedded in flash as const uint8_t arrays generated
// by scripts/compress_fs.py.  Each response uses ~100 B of heap (the
// AsyncCallbackResponse object) instead of ~1500 B (AsyncFileResponse read
// buffer) or the full file size (old malloc approach).  This eliminates the
// OOM crashes that occurred when 6 concurrent HTTP requests exhausted the
// ESP32 heap shared with LVGL, WiFi, MQTT y WebSocket.
//
// En C5 (8 MB PSRAM) tampoco usamos LittleFS para la web: AsyncTCP / AsyncWeb
// reservan sus buffers en heap interno (DMA + mutex constraints), así que
// servir desde LittleFS dispara igualmente OOM con 6+ requests paralelos.
#include "webfiles.h"
#include <atomic>
#include <memory>

// Soft cap on concurrent emitting responses. Browsers open up to 6 parallel
// TCP connections to the same host; after a cache-bust all referenced
// assets invalidate at once and the burst can pressure internal SRAM.
//
// We accept every request — rejecting with 503 is useless because browsers
// do not retry sub-resources — and call send() immediately so the
// framework does not 501 the handler. Backpressure happens inside the
// chunk-fill callback: while too many responses are already emitting data,
// the callback returns RESPONSE_TRY_AGAIN.
//
// Cap=4 (was 2): with LVGL pool moved to PSRAM the internal SRAM headroom
// is comfortable (~60 KB at runtime), so two extra parallel responses
// fit. Cap=2 was too tight under cache-bust reloads — 4 connections sat
// in TRY_AGAIN long enough to interleave with the WebSocket handshake,
// AsyncTCP's queue (CONFIG_ASYNC_TCP_QUEUE_SIZE=8) saturated, and the
// largest response (the PNG, ~7 KB ungzipped) ended up truncated mid-
// stream producing ERR_CONTENT_LENGTH_MISMATCH.
//
// The counter is only mutated from the async_tcp task (fill callback +
// onDisconnect), so no mutex is needed beyond std::atomic for ordering.
static constexpr int MAX_EMITTING_HTTP = 4;
static std::atomic<int> s_emittingHttp{0};

struct ChunkState {
    const uint8_t* data;
    size_t         size;
    bool           acquired = false;   // true once a slot was reserved
};

static void sendMemFile(AsyncWebServerRequest* req,
                        const uint8_t* data, size_t size,
                        const char* ct, bool gzipped)
{
    auto state = std::make_shared<ChunkState>();
    state->data = data;
    state->size = size;

    // Release the slot when the response finishes (sent OR aborted).
    req->onDisconnect([state]() {
        if (state->acquired) {
            s_emittingHttp.fetch_sub(1, std::memory_order_release);
            state->acquired = false;
        }
    });

    AsyncWebServerResponse* r = req->beginResponse(
        ct, size,
        [state](uint8_t* dst, size_t maxLen, size_t idx) -> size_t {
            // First call: try to acquire a slot.
            if (!state->acquired) {
                int prev = s_emittingHttp.fetch_add(1, std::memory_order_acq_rel);
                if (prev >= MAX_EMITTING_HTTP) {
                    // No slot — back off and ask AsyncTCP to retry later.
                    s_emittingHttp.fetch_sub(1, std::memory_order_release);
                    return RESPONSE_TRY_AGAIN;
                }
                state->acquired = true;
            }
            // Emit data.
            if (idx >= state->size) return 0;   // end of stream
            size_t chunk = (maxLen < state->size - idx) ? maxLen : (state->size - idx);
            memcpy(dst, state->data + idx, chunk);
            return chunk;
        });
    if (!r) { req->send(503); return; }
    if (gzipped) r->addHeader("Content-Encoding", "gzip");
    // immutable: browsers will not revalidate during the max-age period,
    // eliminating concurrent requests on page reload.
    r->addHeader("Cache-Control", "max-age=31536000, immutable");
    // Force-close each HTTP connection so the browser cannot keep 6 parallel
    // TCP sockets open.  On ESP32 WROOM + BLE there is not enough contiguous
    // heap to hold that many AsyncClient objects simultaneously.
    r->addHeader("Connection", "close");
    req->send(r);
}

// ═════════════════════════════════════════════════════════════════════════════
// WebSocket
// ═════════════════════════════════════════════════════════════════════════════
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    data[len] = 0;
    String message = (char*)data;
    LOG_WEB_P("Received websocket message ");
    LOG_WEB_PL(message);
    if (message=="settings") {
      // Drain any pending bus messages before sending the init burst,
      // so we don't compete with queued traffic for the per-client queue.
      wsQueueDrain();
      if (wsConn!=NULL) { wsConn(); }
      return;
    }
    if (message=="ping") {
      if (wsCb!=NULL) { wsCb("/ping","1"); }
      return;
    }
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);
    if (error.code()==DeserializationError::Ok) {
      const char *id=doc["id"];
      const char *value=doc["value"];
      if (id && value) {
        if (wsCb!=NULL) { wsCb(id,value); }
      } else {
        LOG_WEB_PL("missing id or value");
      }
    } else {
      LOG_WEB_P("Error decoding json: ");
      LOG_WEB_PL(error.c_str());
    }
  }
}

// Cap the number of concurrent WebSocket clients. Each client costs ~10 KB of
// heap (AsyncTCP control + send/recv buffers). Cap 4 leaves room for browser
// reload overlaps (old WS not yet GC'd while new one opens) without evicting.
// With LVGL pool in PSRAM and BLE memory in check there is enough internal
// SRAM (~60 KB at runtime) to host 4 clients on C5.
static constexpr uint8_t WS_MAX_CLIENTS = 4;

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
             AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      wsClientCount.fetch_add(1);
      if (wsClientCount.load() > WS_MAX_CLIENTS) {
        LOG_WEB_PF("WebSocket client #%u rejected (max %u)\n",
                      client->id(), WS_MAX_CLIENTS);
        client->close(1013, "busy");
        wsClientCount.fetch_sub(1);
        break;
      }
      LOG_WEB_PF("WebSocket client #%u connected from %s  heap=%u\n",
                    client->id(), client->remoteIP().toString().c_str(),
                    ESP.getFreeHeap());
      if (wsCb!=NULL) { wsCb("/ping","1"); }
      break;
    case WS_EVT_DISCONNECT:
      if (wsClientCount.load() > 0) wsClientCount.fetch_sub(1);
      LOG_WEB_PF("WebSocket client #%u disconnected  heap=%u\n",
                    client->id(), ESP.getFreeHeap());
      break;
    case WS_EVT_DATA:
      handleWebSocketMessage(arg, data, len);
      break;
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}

void initWebSocket() {
  ws.onEvent(onEvent);
  server.addHandler(&ws);
}

// ═════════════════════════════════════════════════════════════════════════════
// Thread-safe WS queue (Core 0 -> Core 1)
// ═════════════════════════════════════════════════════════════════════════════

// Inter-task queue between async_tcp publishers and loopTask drain.
// 32 slots: wsConnected() bursts ~25-30 messages (setpoints + frame
// publishers + master frames + solar/batt) in a single async_tcp run
// before loopTask gets to drain. With queue=4 the tail of that burst
// was silently dropped, leaving the page missing room_temp / water_temp
// / heartbeat etc. until the next value change. ~4.5 KB cost is fine
// now that LVGL pool sits in PSRAM.
static constexpr uint8_t  WS_QUEUE_LEN  = 32;
static constexpr uint16_t WS_QUEUE_SIZE = 150;

bool wsQueueSend(const char* msg)
{
    if (!wsQueue || !msg) return false;
    size_t len = strlen(msg);
    if (len >= WS_QUEUE_SIZE) len = WS_QUEUE_SIZE - 1;
    char buf[WS_QUEUE_SIZE];
    memcpy(buf, msg, len);
    buf[len] = '\0';
    return xQueueSend(wsQueue, buf, 0) == pdTRUE;
}

void wsQueueDrain()
{
    if (!wsQueue) return;
    char buf[WS_QUEUE_SIZE];
    while (xQueueReceive(wsQueue, buf, 0) == pdTRUE) {
        ws.textAll(buf);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// StartServer
// ═════════════════════════════════════════════════════════════════════════════
void StartServer(WebsocketCallback cb, WebsocketConnected conn) {
  Serial.println("[web] StartServer: entering");
  wsCb   = cb;
  wsConn = conn;

  // Create the cross-core WS message queue once.
  if (!wsQueue) {
    wsQueue = xQueueCreate(WS_QUEUE_LEN, WS_QUEUE_SIZE);
    Serial.printf("[web] wsQueue created (%d slots x %d bytes)\n", WS_QUEUE_LEN, WS_QUEUE_SIZE);
  }

  initWebSocket();
  Serial.println("[web] initWebSocket done");

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendMemFile(r, wf_index_html, wf_index_html_size, "text/html", wf_index_html_gzipped);
  });
  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendMemFile(r, wf_index_html, wf_index_html_size, "text/html", wf_index_html_gzipped);
  });
  server.on("/styles.css", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendMemFile(r, wf_styles_css, wf_styles_css_size, "text/css", wf_styles_css_gzipped);
  });
  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendMemFile(r, wf_script_js, wf_script_js_size, "application/javascript", wf_script_js_gzipped);
  });
  server.on("/errors.js", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendMemFile(r, wf_errors_js, wf_errors_js_size, "application/javascript", wf_errors_js_gzipped);
  });
  server.on("/i18n.js", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendMemFile(r, wf_i18n_js, wf_i18n_js_size, "application/javascript", wf_i18n_js_gzipped);
  });
  server.on("/reconnecting-websocket.min.js", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendMemFile(r, wf_reconnecting_websocket_min_js,
                   wf_reconnecting_websocket_min_js_size, "application/javascript",
                   wf_reconnecting_websocket_min_js_gzipped);
  });
  server.on("/fa-subset.woff", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendMemFile(r, wf_fa_subset_woff, wf_fa_subset_woff_size, "font/woff", wf_fa_subset_woff_gzipped);
  });
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendMemFile(r, wf_favicon_ico, wf_favicon_ico_size, "image/x-icon", wf_favicon_ico_gzipped);
  });
  server.on("/truminus-logo.png", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendMemFile(r, wf_truminus_logo_png, wf_truminus_logo_png_size, "image/png", wf_truminus_logo_png_gzipped);
  });

  server.on("/fscheck", HTTP_GET, [](AsyncWebServerRequest* req) {
    String body = "Heap libre: " + String(ESP.getFreeHeap()) + " B\n";
    #ifdef CYD_C5
    body += "PSRAM libre: " + String(ESP.getFreePsram()) + " B\n";
    #endif
    body += "Web files served from flash (embedded).\n";
    req->send(200, "text/plain", body);
  });

  server.onNotFound([](AsyncWebServerRequest* req) {
    LOG_WEB_PF("[404] %s\n", req->url().c_str());
    req->send(404, "text/plain", "Not found");
  });

  // DEBUG: log every incoming request before routing (helps diagnose
  // "no response at all" situations).
  server.on("/_alive", HTTP_GET, [](AsyncWebServerRequest* req) {
    LOG_WEB_PL("[web] /_alive hit");
    req->send(200, "text/plain", "alive");
  });

  server.begin();
  Serial.print("[web] server.begin() done — listening on http://");
  Serial.print(WiFi.localIP().toString().c_str());
  Serial.println("/");
}

#endif
