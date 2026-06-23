#include "mode_controller.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>

int fanStrToInt(const char* v) {
    if (!v) return -1;
    if (strcmp(v, "off")  == 0) return 0;
    if (strcmp(v, "eco")  == 0) return 1;
    if (strcmp(v, "high") == 0) return 2;
    int n = atoi(v);
    if (n >= 1 && n <= 10) return n + 2;
    return -1;
}

const char* fanIntToStr(int m) {
    static char lvl[4];
    switch (m) {
        case 0:  return "off";
        case 1:  return "eco";
        case 2:  return "high";
        default:
            if (m >= 3 && m <= 12) { snprintf(lvl, sizeof(lvl), "%d", m - 2); return lvl; }
            return "off";
    }
}

int boilerStrToInt(const char* v) {
    if (!v) return -1;
    if (strcmp(v, "off")   == 0) return 0;
    if (strcmp(v, "eco")   == 0) return 1;
    if (strcmp(v, "high")  == 0) return 2;
    if (strcmp(v, "boost") == 0) return 3;
    return -1;
}

const char* boilerIntToStr(int m) {
    static const char* T[4] = { "off", "eco", "high", "boost" };
    return (m >= 0 && m < 4) ? T[m] : "off";
}

TrumaLinSetpoints derive_mode(const P4ControlState& cs) {
    TrumaLinSetpoints result;
    
    int legacyFan;  // 0=off, -1=eco, -2=high, 1..10=level
    if      (cs.fanMode == 0) legacyFan = 0;
    else if (cs.fanMode == 1) legacyFan = -1;
    else if (cs.fanMode == 2) legacyFan = -2;
    else                      legacyFan = cs.fanMode - 2;   // 1..10

    if (!cs.heatingOn) {
        // Heating off → fan-only modes use the 0x10 base; eco/high stay
        // mapped to 0x11 / 0x12 (Truma still expects those even without heat).
        if      (legacyFan > 0)   result.pumpOrFan = 0x10 | legacyFan;
        else if (legacyFan == -1) result.pumpOrFan = 0x11;
        else if (legacyFan == -2) result.pumpOrFan = 0x12;
        else                      result.pumpOrFan = 0x10;
        result.roomSp = 0.0;
    } else {
        result.roomSp = cs.roomSetpoint;
        // Heating on + numeric fan level: legacy code forced level→eco/high.
        // Keep that compromise — Truma rejects level numbers in heat mode.
        result.pumpOrFan = (legacyFan == -2) ? 2 : 1;
    }

    switch (cs.boilerMode) {
        case 1:  result.waterSp = 40.0; break;
        case 2:  result.waterSp = 60.0; break;
        case 3:  result.waterSp = 60.0; break;   // boost (no waterboost cycle in MVP)
        default: result.waterSp = 0.0;  break;
    }
    
    return result;
}
