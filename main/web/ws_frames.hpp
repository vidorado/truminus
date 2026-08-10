#pragma once
#include <cstddef>
#include <cstdint>
#include "victronble.hpp"
#include "ultimatronble.hpp"
#include "multiplusble.hpp"
#include "tankble.hpp"
#include "openairble.hpp"

// Single definition of every WebSocket frame the firmware emits more than once.
//
// The same payloads go out from two places with different triggers: ws_snapshot
// sends the full state when a browser connects, ws_broadcaster re-sends
// individual frames as values change. Those were byte-identical snprintf calls
// duplicated across both files, kept in sync only by a comment — so a field
// added to one silently never reached clients that were already connected (or
// vice-versa). Formatting lives here now, and both callers share it.
//
// Pure: no ESP-IDF, no globals, no I/O — every input arrives as a parameter, so
// these are compiled and unit-tested natively (test/host/test_ws_frames.cpp).
// Keep it that way; fetching a snapshot is the caller's job.
//
// Every function writes a NUL-terminated JSON object into `buf` and truncates
// rather than overflowing. WS_FRAME_BUF is large enough for all of them.
static constexpr size_t WS_FRAME_BUF = 256;

// Temperature as the web expects it: one decimal, or the "-273" no-data
// sentinel for NaN / <= -200 °C.
void wsFmtTemp(char* out, size_t n, float celsius);

void wsFmtSolar(char* buf, size_t n, const VictronData& v);
void wsFmtBatt (char* buf, size_t n, const UltimatronData& u);
void wsFmtTank (char* buf, size_t n, const TankData& t);
void wsFmtMulti(char* buf, size_t n, const MultiplusData& m);

// `conn` is live-telemetry reachability (not merely a cached frame) and
// `needPair` the rejecting-handshake state; both are tracked outside OpenAirData.
void wsFmtAc(char* buf, size_t n, const OpenAirData& d, bool conn, bool needPair);

// Generic envelopes. `id` is passed WITHOUT the leading slash; the "/" that the
// web protocol expects is added here.
void wsFmtStatus (char* buf, size_t n, const char* id, const char* value);
void wsFmtSetting(char* buf, size_t n, const char* id, const char* value);

// Topbar icon state: id is "ble" or "tunnel".
void wsFmtIcon(char* buf, size_t n, const char* id, int state);
