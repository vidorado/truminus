#include "ws_frames.hpp"
#include <cmath>
#include <cstdio>

// A Multiplus AC power field: a plain integer, or JSON null when the value is
// the VE.Bus "not available" sentinel (inverter off / port not reporting), so
// the web shows no reading instead of a bogus 0 W.
static const char* multiPowerJson(int32_t w, char* scratch, size_t n) {
    if (w == MULTI_POWER_NA) return "null";
    snprintf(scratch, n, "%d", (int)w);
    return scratch;
}

void wsFmtTemp(char* out, size_t n, float celsius) {
    if (!std::isfinite(celsius) || celsius <= -200.0f) snprintf(out, n, "-273");
    else                                               snprintf(out, n, "%.1f", celsius);
}

void wsFmtSolar(char* buf, size_t n, const VictronData& v) {
    snprintf(buf, n,
             "{\"command\":\"solar\",\"valid\":%s,\"state\":%u,"
             "\"pvW\":%d,\"kWh\":%.2f,\"battV\":%.2f,\"battA\":%.2f}",
             v.valid ? "true" : "false",
             (unsigned)v.state, (int)v.pvW, v.kWhToday, v.battV, v.battA);
}

void wsFmtBatt(char* buf, size_t n, const UltimatronData& u) {
    snprintf(buf, n,
             "{\"command\":\"batt\",\"valid\":%s,\"flow\":%s,\"soc\":%u,"
             "\"battV\":%.2f,\"battA\":%.2f}",
             u.valid ? "true" : "false", u.flowValid ? "true" : "false",
             (unsigned)u.soc, u.battV, u.battA);
}

void wsFmtTank(char* buf, size_t n, const TankData& t) {
    snprintf(buf, n, "{\"command\":\"tank\",\"valid\":%s,\"pct\":%u}",
             t.valid ? "true" : "false", (unsigned)t.pct);
}

void wsFmtMulti(char* buf, size_t n, const MultiplusData& m) {
    char inW[12], outW[12];
    snprintf(buf, n,
             "{\"command\":\"multi\",\"valid\":%s,\"state\":%u,"
             "\"ac_in_w\":%s,\"ac_out_w\":%s,"
             "\"batt_v\":%.2f,\"batt_a\":%.1f,"
             "\"ac_in_state\":%u,\"alarm\":%u,\"soc\":%u}",
             m.valid ? "true" : "false",
             (unsigned)m.deviceState,
             multiPowerJson(m.acInW,  inW,  sizeof(inW)),
             multiPowerJson(m.acOutW, outW, sizeof(outW)),
             std::isnan(m.battV) ? 0.0 : m.battV, m.battA,
             (unsigned)m.acInState, (unsigned)m.alarm, (unsigned)m.soc);
}

void wsFmtAc(char* buf, size_t n, const OpenAirData& d, bool conn, bool needPair) {
    snprintf(buf, n,
             "{\"command\":\"ac\",\"valid\":%s,\"conn\":%s,"
             "\"probe1\":%.1f,\"probe2\":%.1f,"
             "\"blower_pct\":%d,\"comp_rpm\":%d,\"errors\":%d,\"needpair\":%s}",
             d.valid ? "true" : "false", conn ? "true" : "false",
             d.probe1C, d.probe2C,
             d.blowerSpeedPct, d.compressorSpeedRpm, d.errors,
             needPair ? "true" : "false");
}

void wsFmtStatus(char* buf, size_t n, const char* id, const char* value) {
    snprintf(buf, n, "{\"command\":\"status\",\"id\":\"/%s\",\"value\":\"%s\"}",
             id, value);
}

void wsFmtSetting(char* buf, size_t n, const char* id, const char* value) {
    snprintf(buf, n, "{\"command\":\"setting\",\"id\":\"/%s\",\"value\":\"%s\"}",
             id, value);
}

void wsFmtIcon(char* buf, size_t n, const char* id, int state) {
    snprintf(buf, n, "{\"command\":\"icon\",\"id\":\"%s\",\"state\":%d}", id, state);
}
