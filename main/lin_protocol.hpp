#pragma once
#include <cstdint>

// Pure LIN low-level frame protocol math (no ESP-IDF deps) so it can be
// host-tested against the real code — see test/host/test_lin_protocol.cpp.
// LinDriver's getProtectedId()/getChecksum() delegate here; only the UART I/O
// stays in lin_driver.cpp.

// Protected Identifier: frame id (6 bits) + the two LIN parity bits.
uint8_t linProtectedId(uint8_t frameId);

// LIN checksum over `data[0..dataLen-1]`. Classic (data only) for diagnostic
// frames (id >= 0x3C, detected from protectedId's low 6 bits); enhanced (PID +
// data) otherwise. Returns the inverted 8-bit carry-folded sum.
uint8_t linChecksum(uint8_t protectedId, const uint8_t* data, uint8_t dataLen);
