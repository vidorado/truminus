#include "webserver.hpp"
#ifdef WEBSERVER

#include <memory>   // std::shared_ptr

// ═════════════════════════════════════════════════════════════════════════
// Mutex para acceso exclusivo a LittleFS
// ─────────────────────────────────────────────────────────────────────────
// LittleFS no es seguro para acceso concurrente desde la tarea AsyncTCP.
// Un único mutex FreeRTOS garantiza que sólo una petición abre/lee un
// fichero a la vez.  La lectura es rápida (< 10 ms para ficheros pequeños),
// por lo que el impacto en la latencia TCP es mínimo.
//
// Flujo por petición:
//   1. xSemaphoreTake  → acceso exclusivo a LittleFS
//   2. open + readBytes → todo en un buffer temporal (malloc)
//   3. close + xSemaphoreGive  → LittleFS libre inmediatamente
//   4. La respuesta HTTP se sirve desde el buffer usando AwsResponseFiller
//   5. El shared_ptr libera el buffer en cuanto la respuesta termina
//
// Así el RAM dinámico es el máximo de ficheros en vuelo simultáneamente,
// que en la práctica es 1-2 (WiFi local es rápido y el mutex serializa
// las lecturas).
// ═════════════════════════════════════════════════════════════════════════
static SemaphoreHandle_t s_lfsMutex = NULL;

static void sendFile(AsyncWebServerRequest* req,
                     const char* gzPath,
                     const char* ct)
{
    // ── 1. Esperar turno ──────────────────────────────────────────────────
    if (xSemaphoreTake(s_lfsMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        Serial.printf("[webfs] timeout esperando mutex para %s\n", gzPath);
        req->send(503, "text/plain", "Servidor ocupado, reintenta");
        return;
    }

    // ── 2. Abrir y leer ───────────────────────────────────────────────────
    File f = LittleFS.open(gzPath, "r");
    if (!f || f.size() == 0) {
        xSemaphoreGive(s_lfsMutex);
        Serial.printf("[webfs] no encontrado: %s\n", gzPath);
        req->send(404, "text/plain", "Not found");
        return;
    }

    size_t n = f.size();
    auto buf = std::shared_ptr<uint8_t>((uint8_t*)malloc(n), free);
    if (!buf) {
        f.close();
        xSemaphoreGive(s_lfsMutex);
        Serial.printf("[webfs] OOM para %s (%u B)\n", gzPath, (unsigned)n);
        req->send(503, "text/plain", "Sin memoria");
        return;
    }

    f.readBytes((char*)buf.get(), n);
    f.close();

    // ── 3. Liberar LittleFS cuanto antes ─────────────────────────────────
    xSemaphoreGive(s_lfsMutex);
    Serial.printf("[webfs] %s → %u B servidos\n", gzPath, (unsigned)n);

    // ── 4. Servir desde RAM (sin acceso adicional a LittleFS) ────────────
    // El lambda captura buf por valor (shared_ptr): mientras el filler
    // exista, el buffer no se libera.  Al destruirse la respuesta se
    // destruye el filler y el shared_ptr decrementa el ref-count → free().
    AsyncWebServerResponse* r = req->beginResponse(
        ct, n,
        [buf, n](uint8_t* dst, size_t maxLen, size_t idx) -> size_t {
            if (idx >= n) return 0;
            size_t chunk = (maxLen < n - idx) ? maxLen : (n - idx);
            memcpy(dst, buf.get() + idx, chunk);
            return chunk;
        }
    );
    r->addHeader("Content-Encoding", "gzip");
    r->addHeader("Cache-Control",    "max-age=86400");
    req->send(r);
}

// ═════════════════════════════════════════════════════════════════════════
// WebSocket
// ═════════════════════════════════════════════════════════════════════════
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

// ═════════════════════════════════════════════════════════════════════════
// StartServer
// ═════════════════════════════════════════════════════════════════════════
void StartServer(WebsocketCallback cb, WebsocketConnected conn) {
  wsCb   = cb;
  wsConn = conn;

  // Crear mutex antes de montar LittleFS
  s_lfsMutex = xSemaphoreCreateMutex();
  if (!s_lfsMutex) {
    Serial.println("**** cannot create LittleFS mutex");
    return;
  }

  if (!LittleFS.begin(false)) {
    Serial.println("**** cannot start LittleFS");
    return;
  }

  // Mostrar espacio libre en LittleFS
  Serial.printf("[webfs] LittleFS OK — heap libre: %u B\n", ESP.getFreeHeap());

  initWebSocket();

  // ── Ficheros estáticos (servidos uno a la vez por el mutex) ───────────
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendFile(r, "/index.html.gz", "text/html");
  });
  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendFile(r, "/index.html.gz", "text/html");
  });
  server.on("/styles.css", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendFile(r, "/styles.css.gz", "text/css");
  });
  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendFile(r, "/script.js.gz", "application/javascript");
  });
  server.on("/errors.js", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendFile(r, "/errors.js.gz", "application/javascript");
  });
  server.on("/reconnecting-websocket.min.js", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendFile(r, "/reconnecting-websocket.min.js.gz", "application/javascript");
  });
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendFile(r, "/favicon.ico.gz", "image/x-icon");
  });
  server.on("/truminus-logo.png", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendFile(r, "/truminus-logo.png.gz", "image/png");
  });

  // ── Diagnóstico ────────────────────────────────────────────────────────
  server.on("/fscheck", HTTP_GET, [](AsyncWebServerRequest* req) {
    String r = "Heap libre: " + String(ESP.getFreeHeap()) + " B\n\nFicheros en LittleFS:\n";
    File root = LittleFS.open("/");
    if (root) {
      File file = root.openNextFile();
      while (file) {
        r += "  ";
        r += file.name();
        r += String("                         ").substring(0, 25 - strlen(file.name()));
        r += String(file.size()) + " B\n";
        file = root.openNextFile();
      }
    }
    req->send(200, "text/plain", r);
  });

  // ── 404 para todo lo demás ─────────────────────────────────────────────
  server.onNotFound([](AsyncWebServerRequest* req) {
    Serial.printf("[404] %s\n", req->url().c_str());
    req->send(404, "text/plain", "Not found");
  });

  server.begin();
}

#endif
