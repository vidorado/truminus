#pragma once
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_http_server.h"

#ifndef NO_MQTT
#include "mqtt_client.h"
extern esp_mqtt_client_handle_t mqttClient;
#endif

static constexpr char BaseTopicStatus[] = "truma/status";
static constexpr char BaseTopicSet[]    = "truma/set";

#define STATUS_TOPIC   "truma/status/online"
#define STATUS_OFFLINE "offline"
#define STATUS_ONLINE  "online"

// ── HTTP / WebSocket ──────────────────────────────────────────────────────
extern httpd_handle_t httpServer;

// Number of active WS connections (updated in webserver.cpp).
extern std::atomic<int> wsClientCount;

// Inter-task WS message queue (linBusTask/BLE → main-task drain).
extern QueueHandle_t wsQueue;

// ── Home Assistant autodiscovery ──────────────────────────────────────────
#define HA_DEVICE_ID           "truma_boiler_01"
#define HA_DEVICE_NAME         "Combi D"
#define HA_DEVICE_MODEL        "Combi Heater"
#define HA_DEVICE_MANUFACTURER "Truma"
#define HA_DISCOVERY_TOPIC     "homeassistant/"

#define HA_FLOAT_MIN -100000.0
#define HA_FLOAT_MAX  100000.0
