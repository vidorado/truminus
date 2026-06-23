#pragma once
#include "p4display.hpp"

// Set initial field values (NaN / default) before the first p4DisplayUpdate.
void displaySyncInit(P4DisplayData& d);

// Gather live data from all subsystems, manage status-line alerts and error
// modals, then call p4DisplayUpdate(d).  Call once per second from app_main.
void displaySyncTick(P4DisplayData& d, uint32_t iter);
