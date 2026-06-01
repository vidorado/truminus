#pragma once
//
// WebSocket reverse-tunnel client.
//
// Connects out to a Plesk-hosted Node.js bridge (server/app.js) over WSS
// and multiplexes browser ↔ ESP TCP streams as JSON frames so the local
// esp_http_server is reachable through CGNAT.  Replaces the previous
// libssh2/serveo backend, which is unportable against ESP-IDF 6 / mbedTLS 4.
//
// Wire protocol (text WS frames, JSON):
//   ESP   → srv  {"type":"hello","node":"truminus","token":"<shared>"}
//   srv   → ESP  {"type":"open", "id":<n>}
//   both         {"type":"data", "id":<n>, "b":"<base64-bytes>"}
//   both         {"type":"close","id":<n>}
//
// Per `id` the ESP keeps one TCP socket to 127.0.0.1:80.  Bytes flow opaquely
// in both directions; HTTP requests and inner /ws upgrades are never parsed
// by the tunnel.
//
// NVS namespace "tunnel":
//   - "enabled" (u8)  : 0/1
//   - "server"  (str) : "wss://tunel.example.com/tunnel" (full URL)
//   - "token"   (str) : shared secret matching TUNNEL_TOKEN on the server

#include <stdint.h>
#include <stddef.h>

struct TunnelConfig {
    bool enabled;
    char server[160];   // full URL, e.g. "wss://tunel.victordorado.es/tunnel"
    char token[96];     // shared secret
};

// Load config from NVS + spawn worker task (no-op if disabled).  Safe to
// call once from bootTask after WiFi has an IP.
//
// deferConnect: set up the task + handlers but DON'T open the connection yet,
// even if enabled.  Used on a freshly-OTA'd (PENDING_VERIFY) boot so the TLS
// handshake's internal-DRAM/SRAM spike doesn't coincide with WiFi/BLE bring-up
// during the self-test heap-floor window.  The icon stays CONNECTING; the OTA
// self-test calls wstunnelApply() to bring it up once the image is valid.
void wstunnelInit(bool deferConnect = false);

void wstunnelLoadConfig(TunnelConfig& out);
void wstunnelSaveConfig(const TunnelConfig& cfg);

// Apply the saved config to the running task (start / stop / reconnect with
// new server URL).  Non-blocking.
void wstunnelApply();

// Tear the WS client down without changing the saved config — used to free
// internal DRAM for the TLS handshake during a firmware OTA.  The tunnel
// stays "enabled"; call wstunnelApply() to bring it back (or just reboot).
void wstunnelSuspend();

// True between the ESP-side WS "open" callback and the corresponding close.
bool wstunnelIsConnected();

// UI state for the topbar cloud icon.  Mirrors the connection lifecycle of
// the websocket client so the icon can be solid blue / blinking / red /
// grey without the display task needing access to module internals.
enum class TunnelUiState : uint8_t {
    DISABLED   = 0,  // tunnel is OFF in settings — grey icon
    CONNECTING = 1,  // start_client called, no "connected" event yet — blink
    CONNECTED  = 2,  // last event was CONNECTED — solid blue
    FAILED     = 3,  // 3+ consecutive disconnects without a CONNECTED in between — red
};
TunnelUiState wstunnelUiState();
