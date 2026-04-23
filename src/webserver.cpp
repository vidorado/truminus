#include "webserver.hpp"
#ifdef WEBSERVER
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    data[len] = 0;
    String message = (char*)data;
    Serial.print("Received websocket message ");
    Serial.println(message);
    if (message=="settings") {
      if (wsConn!=NULL) {
        wsConn();
      }
      return; 
    }
    if (message=="ping"){
      if (wsCb!=NULL) {
        wsCb("/ping","1");
      }
      return;
    }
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);
    if (error.code()==DeserializationError::Ok) {
      const char *id=doc["id"];
      const char *value=doc["value"];
      if (id && value) { 
        if (wsCb!=NULL) {
          wsCb(id,value);
        }
      } else {
        Serial.println("missing id or value");
      }
    } else {
      Serial.print("Error decoding json: ");
      Serial.println(error.c_str());
    }
  }  
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      if (wsCb!=NULL) {
        wsCb("/ping","1");
      }
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

// ── MIME type desde la extensión del fichero original (sin .gz) ──────────
static String mimeFor(const String& path) {
  if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".ico"))  return "image/x-icon";
  if (path.endsWith(".svg"))  return "image/svg+xml";
  if (path.endsWith(".webp")) return "image/webp";
  return "application/octet-stream";
}

void StartServer(WebsocketCallback cb,  WebsocketConnected conn) {
  wsCb=cb;
  wsConn=conn;
  if (LittleFS.begin(false)) {
    Serial.printf("Starting webserver — free heap: %u bytes\n", ESP.getFreeHeap());

    initWebSocket();

    // ── Diagnóstico: GET /fscheck ──────────────────────────────────────────
    server.on("/fscheck", HTTP_GET, [](AsyncWebServerRequest *request) {
      static const char* files[] = {
        "/index.html",                      "/index.html.gz",
        "/styles.css",                      "/styles.css.gz",
        "/script.js",                       "/script.js.gz",
        "/errors.js",                       "/errors.js.gz",
        "/reconnecting-websocket.min.js",   "/reconnecting-websocket.min.js.gz",
        "/favicon.ico",                     "/favicon.ico.gz",
        "/truminus-logo.png",               "/truminus-logo.png.gz",
      };
      String r;
      for (auto& f : files)
        r += (LittleFS.exists(f) ? "[Y] " : "[N] ") + String(f) + "\n";
      r += "\nFree heap: " + String(ESP.getFreeHeap()) + " bytes\n";
      request->send(200, "text/plain", r);
    });

    // ── Ficheros estáticos desde LittleFS ────────────────────────────────
    // serveStatic calcula mal Content-Length al servir .gz; usamos
    // beginResponse() directamente sobre el .gz — la librería añade
    // Content-Encoding: gzip y mide el tamaño del fichero real.
    server.onNotFound([](AsyncWebServerRequest *request) {
      if (request->method() != HTTP_GET) {
        request->send(405, "text/plain", "Method Not Allowed");
        return;
      }
      String path = request->url();
      if (path == "/" || path.endsWith("/")) path += "index.html";
      if (path.indexOf("..") >= 0) { request->send(400); return; }

      String gzPath = path + ".gz";
      if (LittleFS.exists(gzPath)) {
        AsyncWebServerResponse *r =
            request->beginResponse(LittleFS, gzPath, mimeFor(path));
        r->addHeader("Cache-Control", "max-age=86400");
        request->send(r);
        return;
      }
      if (LittleFS.exists(path)) {
        request->send(LittleFS, path, mimeFor(path));
        return;
      }
      Serial.printf("[404] %s\n", request->url().c_str());
      request->send(404, "text/plain", "Not found");
    });

    server.begin();
  } else {
    Serial.println("**** cannot start LittleFS");
  }
}

#endif
