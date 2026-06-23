#pragma once
#ifdef WEBSERVER

#include <atomic>
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ── Public API ───────────────────────────────────────────────────────────────

// Mount the LittleFS partition under `/littlefs`. Call once at boot, before
// startWebServer().  Returns ESP_OK on success.  Safe to call twice
// (second call is a no-op).
esp_err_t mountWebFs();

// Unmount the LittleFS (self-OTA web-asset rewrite).  No-op if not mounted;
// call mountWebFs() again to remount after the partition is rewritten.
esp_err_t unmountWebFs();

// WebSocket command callback.
//   id    : "/heating", "/temp", "/fan", ...  (with leading slash, as on C5)
//   value : raw string value sent by the browser ("on" / "21.5" / etc.)
// Invoked once per incoming {id,value} JSON frame.
typedef void (*WsCommandCb)(const char* id, const char* value);

// WebSocket "settings" (initial snapshot) callback.  Browsers send the text
// frame "settings" right after connecting; the server invokes this so the
// app layer can push a snapshot of every cached setting/status via
// wsQueueSend().
typedef void (*WsConnectedCb)(void);

// Start the HTTP + WebSocket server.  cb may be nullptr; conn may be nullptr.
// Returns ESP_OK if the server was successfully started.
esp_err_t startWebServer(WsCommandCb cb, WsConnectedCb conn);

// Stop the server and free internal resources.  Safe to call if not started.
void stopWebServer();

// Enqueue a WebSocket text message for broadcast to every connected client.
// Safe to call from any FreeRTOS task (LIN task on core 0, BLE supervisor,
// main loop).  The message is copied into an internal queue; no heap
// allocation occurs.  Returns true if queued, false if the queue is full or
// the server is not running.
bool wsQueueSend(const char* msg);

// Drain every queued message and broadcast it.  Must be called periodically
// from the main task (or any task that does not hold the httpd lock).
void wsQueueDrain();

// Globals shared with the rest of the firmware (defined in webserver.cpp).
extern std::atomic<int> wsClientCount;
extern QueueHandle_t    wsQueue;
extern httpd_handle_t   httpServer;

#endif // WEBSERVER
