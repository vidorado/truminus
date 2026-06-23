#pragma once

// Pure reset-reason classification (no ESP-IDF deps) so it can be host-tested
// against the real code — see test/host/test_faultlog.cpp. Operates on the
// integer value of esp_reset_reason_t (a stable ESP-IDF enum); the NVS record
// keeping stays in faultlog.cpp.

// Synthetic reason code (outside the real esp_reset_reason_t range) for an OTA
// self-test rollback, so it can share the faultlog "diag" record and surface in
// the same UI line. Firmware that knows this code renders "OTA rollback";
// firmware that predates it renders "unknown" but still shows the detail packed
// into the version field.
#define FAULT_RSN_OTA_ROLLBACK 200

// True when the reset reason means the code actually crashed (panic / watchdog),
// as opposed to a controlled restart or a power event. Only crashes update the
// stored record so real bugs aren't masked.
bool faultIsCrash(int resetReason);

// Human-readable name for an esp_reset_reason_t value.
const char* faultReasonName(int reason);
