#pragma once

// Build and queue the current OTA status frame.  Shared by the on-connect
// snapshot and the live broadcaster so both emit an identical envelope.
void wsSendOtaFrame();

// Push a full state snapshot to a browser that just connected.  Covers every
// field the change broadcaster only emits on change, so stale pages catch up.
void wsOnConnected();
