#pragma once
#ifdef WEBSERVER
#include "LittleFS.h"
#include <ArduinoJson.h>
#include <atomic>
#include "globals.hpp"

typedef std::function<void(const String &topicStr, const String &message)> WebsocketCallback;
typedef std::function<void()> WebsocketConnected;

inline AsyncWebServer server(80);
inline WebsocketCallback wsCb=NULL;
inline WebsocketConnected wsConn=NULL;

// Thread-safe queue for WS messages sent from Core 0 (linBusTask)
// Core 1 loop() drains it and calls ws.textAll().
inline QueueHandle_t wsQueue = nullptr;

// Thread-safe WebSocket client counter (avoids calling ws.count() from loop,
// which races with the tcpip_thread that modifies the internal client list).
inline std::atomic<uint8_t> wsClientCount{0};

void StartServer(WebsocketCallback cb, WebsocketConnected conn);

// Enqueue a WS message from Core 0. Safe to call from any task.
// The message is copied into an internal static buffer; no heap allocation occurs.
bool wsQueueSend(const char* msg);

// Drain all queued WS messages and send them. Must be called from Core 1 (loop).
void wsQueueDrain();

#endif 
