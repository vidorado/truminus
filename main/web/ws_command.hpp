#pragma once

// Pure parsing/validation of incoming WebSocket command frames (no ESP-IDF
// deps) so it can be host-tested against the real code — see
// test/host/test_ws_command.cpp.
//
// The web UI sends {id,value} frames (the browser prefixes ids with '/', e.g.
// "/heating"). This layer turns a raw id+value into a validated, dispatch-ready
// WsCommand WITHOUT touching control state, queues or hardware; main.cpp applies
// the result via the p4Set* setters.

enum class WsCmdKind {
    None,        // null id/value
    Unknown,     // recognised nothing — caller logs it
    Heating,     // boolVal
    Fan,         // intVal (fan mode, see mode_controller)
    Boiler,      // intVal (boiler mode, see mode_controller)
    Temp,        // floatVal (room setpoint °C)
    EnergyIdx,   // intVal (0..4)
    AcMode,      // intVal (0=off, 1=cool, 2=eco) — OpenAir A/C
    AcFanAuto,   // boolVal — A/C fan Auto(true)/Man(false)
    AcFanSpeed,  // intVal (1..6) — A/C blower speed in Man
    OtaCheck,    // action, no value
    OtaInstall,  // action, no value
    OtaCancel,   // action, no value
};

struct WsCommand {
    WsCmdKind kind = WsCmdKind::None;
    bool  valid    = false;  // value passed validation (Fan/Boiler can reject)
    bool  boolVal  = false;
    int   intVal   = 0;
    float floatVal = 0.0f;
};

// Parse a raw WebSocket command. A leading '/' on id is stripped. For Fan/Boiler
// `valid` reflects whether the value mapped to a known mode; for the other typed
// commands `valid` is always true (value is parsed best-effort, matching the
// legacy strtof/atoi behavior). Action commands carry no value.
WsCommand parseWsCommand(const char* id, const char* value);
