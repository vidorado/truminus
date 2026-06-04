#include "ws_command.hpp"
#include "mode_controller.hpp"
#include <cstring>
#include <cstdlib>

WsCommand parseWsCommand(const char* id, const char* value) {
    WsCommand out;
    if (!id || !value) return out;  // kind = None

    // Browser sends "/heating"; the route table uses bare names.
    const char* k = (id[0] == '/') ? id + 1 : id;

    if (strcmp(k, "heating") == 0) {
        out.kind = WsCmdKind::Heating;
        out.boolVal = (strcmp(value, "1") == 0) || (strcasecmp(value, "true") == 0);
        out.valid = true;
    } else if (strcmp(k, "fan") == 0) {
        out.kind = WsCmdKind::Fan;
        out.intVal = fanStrToInt(value);
        out.valid = (out.intVal >= 0);
    } else if (strcmp(k, "boiler") == 0) {
        out.kind = WsCmdKind::Boiler;
        out.intVal = boilerStrToInt(value);
        out.valid = (out.intVal >= 0);
    } else if (strcmp(k, "temp") == 0) {
        out.kind = WsCmdKind::Temp;
        out.floatVal = strtof(value, nullptr);
        out.valid = true;
    } else if (strcmp(k, "energy_idx") == 0) {
        out.kind = WsCmdKind::EnergyIdx;
        out.intVal = atoi(value);
        out.valid = true;
    } else if (strcmp(k, "ota_check") == 0) {
        out.kind = WsCmdKind::OtaCheck;
        out.valid = true;
    } else if (strcmp(k, "ota_install") == 0) {
        out.kind = WsCmdKind::OtaInstall;
        out.valid = true;
    } else if (strcmp(k, "ota_cancel") == 0) {
        out.kind = WsCmdKind::OtaCancel;
        out.valid = true;
    } else {
        out.kind = WsCmdKind::Unknown;
    }
    return out;
}
