#pragma once
//
// Truma Combi D — LIN bus scheduler (IDF-native, MVP).
//
// Drives the LIN bus by emulating a CP-Plus D control unit: writes 7 setpoint
// frames + 1 control frame, alternates two master-request frames (OnOff,
// GetErrorInfo), and reads the two status frames (0x21 room/water temp,
// 0x22 water-heating).  No MQTT, no autodiscovery — values are exposed
// through a single shared snapshot that the LCD / WebSocket main loop polls.
//
// Setpoints come from p4GetControlState() — that is, whatever the LCD or the
// WebSocket dispatcher last wrote.  Wakeup → on/off logic: turn on when
// heating, boiler or fan is active; linger 20 s after the last activity
// before sending the off command.
//

#include <cstdint>

struct TrumaLinSnapshot {
    bool    linOk;          // last 0x21 read succeeded recently
    float   roomTemp;       // °C from 0x21 (-273 → no data)
    float   waterTemp;      // °C from 0x21 (-273 → no data)
    bool    waterHeating;   // burner active (from 0x22)
    uint8_t errClass;       // last GetErrorInfo reply (0 = no error)
    uint8_t errCode;
    uint8_t requestedState; // last OnOff requested state byte
    uint8_t currentState;   // last OnOff current state byte
};

// Spawn the LIN task pinned to Core 0.  `tx_pin` / `rx_pin` are the GPIOs
// wired to the LIN transceiver (J5 on the JC4880-P4 board).  Safe to call
// once at boot; subsequent calls are no-ops.
void trumaLinStart(int tx_pin, int rx_pin);

// Thread-safe snapshot accessor for the main loop / WS pump.
//
// Returns false and leaves `out` UNTOUCHED if the lock could not be taken, so
// the caller keeps its last known values. It must not report a zeroed snapshot
// on contention: linOk would read false and the temperatures as no-data, which
// is indistinguishable from a genuinely dead bus — momentary contention would
// flash "No LIN bus" on the LCD and push linok=0 to every browser.
bool trumaLinGetSnapshot(TrumaLinSnapshot& out);
