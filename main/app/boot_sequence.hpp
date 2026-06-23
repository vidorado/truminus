#pragma once

// Spawn the background boot task.  Returns immediately; init runs in parallel
// with the splash so the user sees pixels as soon as the panel is up.
void bootStart(void (*wsCommandCb)(const char*, const char*), void (*wsConnectedCb)());

// Subsystem "attempt started" flags — dim until the subsystem is actually
// trying to connect (see p4display topbar icons).
bool bootWifiAttempting();
bool bootLinAttempting();
bool bootBleAttempting();
