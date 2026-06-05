#pragma once
// Records the last *uncontrolled* reset (panic/abort/watchdog) in NVS so it
// survives controlled reboots (OTA, settings) and power cuts (power-on /
// brownout) — those never overwrite the stored fault.  Surfaced in the serial
// boot log and the web About overlay (see data/script.js `diag` frame).
#include "esp_system.h"
#include "faultlog_codec.hpp"   // faultReasonName(), faultIsCrash()
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

// Record an OTA self-test rollback into the SAME "diag" record a crash uses, so
// it surfaces in the existing About-overlay line — crucially visible even on the
// older image the device rolls back into (it reads the stable f_rsn/f_cnt/f_ver
// keys). `why` + free internal DRAM are packed into the version field; the reason
// code is the synthetic FAULT_RSN_OTA_ROLLBACK (older firmware renders it as
// "unknown" but still shows the packed detail).
void faultLogRecordRollback(const char* why, uint32_t freeInternal);

// Clear the record only if it currently holds an OTA-rollback marker (leaves a
// real crash record intact). Called after a successful OTA self-test.
void faultLogClearRollback(void);
