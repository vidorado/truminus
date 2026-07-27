#pragma once

// Aggregate BLE health for the topbar icon, shared by the LCD (display_sync),
// the WS change-broadcaster and the WS connect snapshot.  All three derived it
// from the same five-way expression over every BLE driver; adding a peripheral
// meant editing three copies, and missing one made the icon disagree between
// the LCD and the web.
//
//   2 = at least one configured peripheral is reporting data
//   1 = something is configured but nothing has reported yet (UI blinks)
//   0 = no BLE peripheral configured at all (UI dims)
//
// A not-yet-connected peripheral is normal (the supervisor polls cyclically),
// so there is deliberately no "failed" state.
int bleIconState();
