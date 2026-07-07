#include "ws_router.hpp"
#include "ws_command.hpp"
#include "p4display.hpp"
#include "p4_ota.hpp"
#include "openairble.hpp"
#include "openair_config.hpp"
#include "esp_log.h"

static const char* TAG = "ws_router";

void wsOnCommand(const char* id, const char* value) {
    // Parsing/validation is pure and host-tested in ws_command.cpp; this layer
    // only dispatches the result to the p4Set*/OTA setters.
    WsCommand cmd = parseWsCommand(id, value);
    switch (cmd.kind) {
        case WsCmdKind::Heating:    p4SetHeating(cmd.boolVal); break;
        case WsCmdKind::Fan:        if (cmd.valid) p4SetFanMode(cmd.intVal); break;
        case WsCmdKind::Boiler:     if (cmd.valid) p4SetBoilerMode(cmd.intVal); break;
        case WsCmdKind::Temp:       p4SetRoomSetpoint(cmd.floatVal); break;
        case WsCmdKind::EnergyIdx:  p4SetEnergyIdx(cmd.intVal); break;
        case WsCmdKind::AcMode:     if (cmd.valid) p4SetAcMode(cmd.intVal); break;
        case WsCmdKind::AcFanAuto: {
            P4ControlState cs; p4GetControlState(cs);
            p4SetAcFan(cmd.boolVal, cs.acFanSpeed);
            break;
        }
        case WsCmdKind::AcFanSpeed: {
            if (!cmd.valid) break;
            P4ControlState cs; p4GetControlState(cs);
            p4SetAcFan(cs.acFanAuto, cmd.intVal);
            break;
        }
        case WsCmdKind::OtaCheck:   p4OtaCheckNow(); break;
        case WsCmdKind::OtaInstall: p4OtaInstall(); break;
        case WsCmdKind::OtaCancel:  p4OtaCancel(); break;
        case WsCmdKind::Unknown:    ESP_LOGI(TAG, "ws cmd (unhandled): %s = %s", id, value); break;
        case WsCmdKind::None:       break;
    }
}
