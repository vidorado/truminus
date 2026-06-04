#pragma once

// Pure reset-reason classification (no ESP-IDF deps) so it can be host-tested
// against the real code — see test/host/test_faultlog.cpp. Operates on the
// integer value of esp_reset_reason_t (a stable ESP-IDF enum); the NVS record
// keeping stays in faultlog.cpp.

// True when the reset reason means the code actually crashed (panic / watchdog),
// as opposed to a controlled restart or a power event. Only crashes update the
// stored record so real bugs aren't masked.
bool faultIsCrash(int resetReason);

// Human-readable name for an esp_reset_reason_t value.
const char* faultReasonName(int reason);
