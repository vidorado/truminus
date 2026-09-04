#include "ws_diff.hpp"
#include <cmath>
#include <cstdlib>

bool multiPowerChanged(int32_t a, int32_t b) {
    if (a == MULTI_POWER_NA || b == MULTI_POWER_NA) return a != b;
    return std::abs(a - b) > 5;
}

bool linTempChanged(float a, float b) {
    bool aBad = !std::isfinite(a) || a <= -200.0f;
    bool bBad = !std::isfinite(b) || b <= -200.0f;
    if (aBad != bBad) return true;
    if (aBad && bBad) return false;
    return fabsf(a - b) > 0.05f;
}

bool victronChanged(const VictronData& c, const VictronData& p) {
    return c.valid != p.valid
        || (c.valid && (c.state != p.state
            || fabsf(c.battV    - p.battV)    > 0.05f
            || fabsf(c.battA    - p.battA)    > 0.05f
            || fabsf(c.pvW      - p.pvW)      > 0.5f
            || fabsf(c.kWhToday - p.kWhToday) > 0.01f));
}

bool ultimatronChanged(const UltimatronData& c, const UltimatronData& p) {
    return c.valid != p.valid
        || c.flowValid != p.flowValid   // re-emit when the power/flow blanks or returns
        || (c.valid && (c.soc != p.soc
            || fabsf(c.battV - p.battV) > 0.05f
            || fabsf(c.battA - p.battA) > 0.05f));
}

bool multiplusChanged(const MultiplusData& c, const MultiplusData& p) {
    return c.valid != p.valid
        || (c.valid && (c.deviceState != p.deviceState
            || c.alarm     != p.alarm
            || c.acInState != p.acInState
            || multiPowerChanged(c.acInW,  p.acInW)
            || multiPowerChanged(c.acOutW, p.acOutW)
            || (!std::isnan(c.battV) && fabsf(c.battV - p.battV) > 0.05f)
            || fabsf(c.battA - p.battA) > 0.1f
            || c.battTempC != p.battTempC
            || c.soc != p.soc));
}

bool tankChanged(const TankData& c, const TankData& p) {
    // Re-emit on any pct change AND on every fresh advert (lastMs moves) so the
    // level can be watched rising smoothly while filling.
    return c.valid != p.valid
        || (c.valid && c.pct != p.pct)
        || (c.valid && c.lastMs != p.lastMs);
}
