#pragma once
#include <cstdint>
#include "victronble.hpp"
#include "ultimatronble.hpp"
#include "multiplusble.hpp"
#include "tankble.hpp"

// Change-detection predicates for the WebSocket broadcasters (the "diff" half of
// ws_broadcaster.cpp's broadcast* functions). Pure (no ESP-IDF deps) so the
// threshold / NaN / sentinel edge cases are host-tested against the real code —
// see test/host/test_ws_diff.cpp. JSON formatting lives in ws_frames.cpp and the
// wsQueue enqueue in ws_broadcaster.cpp; these only answer "did this value
// change enough to re-broadcast?".
//
// Each predicate encodes the field comparison only; ws_broadcaster.cpp still
// gates the first emit with its own `!inited` flag.

// True when two AC power readings differ enough to rebroadcast. Treats the
// MULTI_POWER_NA "no data" sentinel as its own state and avoids the abs()
// overflow a plain delta on INT32_MIN would cause.
bool multiPowerChanged(int32_t a, int32_t b);

// NaN/sentinel-aware temperature compare (<= -200 °C or non-finite = "no data").
// A transition into/out of "no data" counts as changed; otherwise 0.05 °C thresh.
bool linTempChanged(float a, float b);

bool victronChanged(const VictronData& cur, const VictronData& prev);
bool ultimatronChanged(const UltimatronData& cur, const UltimatronData& prev);
bool multiplusChanged(const MultiplusData& cur, const MultiplusData& prev);
bool tankChanged(const TankData& cur, const TankData& prev);
