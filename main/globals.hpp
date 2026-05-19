#pragma once

#ifndef NO_MQTT
#include "mqtt_client.h"
extern esp_mqtt_client_handle_t mqttClient;
#endif

static constexpr char BaseTopicStatus[] = "truma/status";
static constexpr char BaseTopicSet[]    = "truma/set";

#define STATUS_TOPIC   "truma/status/online"
#define STATUS_OFFLINE "offline"
#define STATUS_ONLINE  "online"

// HTTP / WebSocket globals (httpServer, wsClientCount, wsQueue) live in
// webserver.hpp and are only declared when WEBSERVER is defined.

// ── Home Assistant autodiscovery ──────────────────────────────────────────
#define HA_DEVICE_ID           "truma_boiler_01"
#define HA_DEVICE_NAME         "Combi D"
#define HA_DEVICE_MODEL        "Combi Heater"
#define HA_DEVICE_MANUFACTURER "Truma"
#define HA_DISCOVERY_TOPIC     "homeassistant/"

#define HA_FLOAT_MIN -100000.0
#define HA_FLOAT_MAX  100000.0
