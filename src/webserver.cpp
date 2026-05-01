#include "webserver.hpp"
#ifdef WEBSERVER

// Static web files are embedded in flash as const uint8_t arrays generated
// by scripts/compress_fs.py.  Each response uses ~100 B of heap (the
// AsyncCallbackResponse object) instead of ~1500 B (AsyncFileResponse read
// buffer) or the full file size (old malloc approach).  This eliminates the
// OOM crashes that occurred when 6 concurrent HTTP requests exhausted the
// ESP32 heap shared with LVGL, WiFi, MQTT and WebSocket.
#include "webfiles.h"

static void sendMemFile(AsyncWebServerRequest* req,
                        const uint8_t* data, size_t size,
                        const char* ct, bool gzipped)
{
    AsyncWebServerResponse* r = req->beginResponse(
        ct, size,
        [data, size](uint8_t* dst, size_t maxLen, size_t idx) -> size_t {
            if (idx >= size) return 0;
            size_t chunk = (maxLen < size - idx) ? maxLen : (size - idx);
            memcpy(dst, data + idx, chunk);
            return chunk;
        });
    if (!r) { req->send(503); return; }
    if (gzipped) r->addHeader("Content-Encoding", "gzip");
    r->addHeader("Cache-Control", "max-age=86400");
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
    Serial.print("Received websocket message ");
    Serial.println(message);
    if (message=="settings") {
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
        Serial.println("missing id or value");
      }
    } else {
      Serial.print("Error decoding json: ");
      Serial.println(error.c_str());
    }
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
             AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n",
                    client->id(), client->remoteIP().toString().c_str());
      if (wsCb!=NULL) { wsCb("/ping","1"); }
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
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
// StartServer
// ═════════════════════════════════════════════════════════════════════════════
void StartServer(WebsocketCallback cb, WebsocketConnected conn) {
  wsCb   = cb;
  wsConn = conn;

  initWebSocket();

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
    req->send(200, "text/plain",
              "Heap libre: " + String(ESP.getFreeHeap()) + " B\n"
              "Web files served from flash (no LittleFS needed for HTTP).\n");
  });

  server.onNotFound([](AsyncWebServerRequest* req) {
    Serial.printf("[404] %s\n", req->url().c_str());
    req->send(404, "text/plain", "Not found");
  });

  server.begin();
}

#endif
