#pragma once
// Records the last *uncontrolled* reset (panic/abort/watchdog) in NVS so it
// survives controlled reboots (OTA, settings) and power cuts (power-on /
// brownout) — those never overwrite the stored fault.  Surfaced in the serial
// boot log and the web About overlay (see data/script.js `diag` frame).
#include "esp_system.h"
#include <cstdint>

struct FaultInfo {
    esp_reset_reason_t reason;     // stored fault cause
    uint32_t           count;      // cumulative uncontrolled faults
    char               version[32]; // firmware version when it last happened
};

// Read esp_reset_reason(), log it, and update the NVS record when this boot
// followed an uncontrolled fault.  Call once, early in app_main (after NVS).
void faultLogInit();

// Last recorded uncontrolled fault; false if none has ever been recorded.
bool faultLogGet(FaultInfo& out);

// Human-readable name for an esp_reset_reason_t value.
const char* faultReasonName(int reason);
