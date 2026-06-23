#pragma once
#include <stddef.h>

// Returns true if the C6 co-processor firmware version does not match the
// host ESP-Hosted library version (major.minor comparison; patch is ignored).
// If slave_ver is non-null, fills it with the current C6 version string
// ("X.Y.Z") so the caller can display it; writes "?" on failure.
// Must be called after the ESP-Hosted transport is established (i.e. after
// wifi_manager_init()).
bool c6OtaNeeded(char* slave_ver = nullptr, size_t slave_ver_len = 0);

// Streams the embedded C6 firmware binary to the co-processor via SDIO OTA.
// progress_cb(pct) is called each time progress crosses a 5% boundary; pass
// nullptr to skip progress reporting.
// Returns true on success; the caller must call esp_restart() afterwards.
// Returns false without side-effects when C6_FW_AVAILABLE is not defined
// (the binary was not embedded — run 'make build-c6' then rebuild).
bool c6OtaPerform(void (*progress_cb)(int) = nullptr);
