// Tests for main/web/ws_frames.cpp — the WebSocket frame formatters shared by
// the connect snapshot (ws_snapshot.cpp) and the change broadcaster
// (ws_broadcaster.cpp).
//
// The payloads are a wire contract with data/script.js, so these lock down the
// exact JSON: a renamed key or a changed numeric precision silently breaks the
// web UI, and nothing else in the build would catch it.

#include "catch_amalgamated.hpp"
#include "ws_frames.hpp"
#include <cmath>
#include <cstring>
#include <string>

namespace {
std::string solar(const VictronData& v) {
    char b[WS_FRAME_BUF]; wsFmtSolar(b, sizeof(b), v); return b;
}
std::string batt(const UltimatronData& u) {
    char b[WS_FRAME_BUF]; wsFmtBatt(b, sizeof(b), u); return b;
}
std::string tank(const TankData& t) {
    char b[WS_FRAME_BUF]; wsFmtTank(b, sizeof(b), t); return b;
}
std::string multi(const MultiplusData& m) {
    char b[WS_FRAME_BUF]; wsFmtMulti(b, sizeof(b), m); return b;
}
std::string ac(const OpenAirData& d, bool conn, bool needPair) {
    char b[WS_FRAME_BUF]; wsFmtAc(b, sizeof(b), d, conn, needPair); return b;
}
std::string temp(float c) {
    char b[16]; wsFmtTemp(b, sizeof(b), c); return b;
}
}  // namespace

TEST_CASE("wsFmtTemp maps no-data to the -273 sentinel", "[ws_frames]") {
    CHECK(temp(21.5f)   == "21.5");
    CHECK(temp(-12.34f) == "-12.3");   // one decimal, rounded
    CHECK(temp(0.0f)    == "0.0");

    // The web treats -273 as "no reading"; every unusable value must map to it.
    CHECK(temp(NAN)                                  == "-273");
    CHECK(temp(-273.0f)                              == "-273");
    CHECK(temp(-200.0f)                              == "-273");   // boundary is inclusive
    CHECK(temp(std::numeric_limits<float>::infinity()) == "-273");

    CHECK(temp(-199.9f) == "-199.9");  // just inside the valid range
}

TEST_CASE("wsFmtSolar emits the solar contract", "[ws_frames]") {
    VictronData v{};
    v.battV = 13.25f; v.battA = 4.5f; v.pvW = 123.7f;
    v.kWhToday = 1.234f; v.state = 3; v.valid = true;

    CHECK(solar(v) ==
          "{\"command\":\"solar\",\"valid\":true,\"state\":3,"
          "\"pvW\":123,\"kWh\":1.23,\"battV\":13.25,\"battA\":4.50}");

    VictronData off{};
    CHECK(solar(off).find("\"valid\":false") != std::string::npos);
}

TEST_CASE("wsFmtBatt reports SOC and flow freshness independently", "[ws_frames]") {
    UltimatronData u{};
    u.soc = 87; u.battV = 13.4f; u.battA = -12.5f;
    u.valid = true; u.flowValid = true;

    CHECK(batt(u) ==
          "{\"command\":\"batt\",\"valid\":true,\"flow\":true,\"soc\":87,"
          "\"battV\":13.40,\"battA\":-12.50}");

    // SOC survives its long window while the instantaneous flow has expired —
    // the two flags must not collapse into one.
    u.flowValid = false;
    CHECK(batt(u).find("\"valid\":true,\"flow\":false") != std::string::npos);
}

TEST_CASE("wsFmtTank emits the tank contract", "[ws_frames]") {
    TankData t{}; t.pct = 42; t.valid = true;
    CHECK(tank(t) == "{\"command\":\"tank\",\"valid\":true,\"pct\":42}");

    TankData none{};
    CHECK(tank(none) == "{\"command\":\"tank\",\"valid\":false,\"pct\":0}");
}

TEST_CASE("wsFmtMulti renders the NA power sentinel as JSON null", "[ws_frames]") {
    MultiplusData m{};
    m.deviceState = 9; m.acInW = 230; m.acOutW = 450;
    m.battV = 13.1f; m.battA = -8.25f;
    m.acInState = 0; m.alarm = 0; m.soc = 77; m.valid = true;

    CHECK(multi(m) ==
          "{\"command\":\"multi\",\"valid\":true,\"state\":9,"
          "\"ac_in_w\":230,\"ac_out_w\":450,"
          "\"batt_v\":13.10,\"batt_a\":-8.2,"
          "\"ac_in_state\":0,\"alarm\":0,\"soc\":77}");

    // A port at rest reports the sentinel; it must become null, never a number
    // (0 W and "not reporting" mean different things to the web UI).
    m.acInW = MULTI_POWER_NA;
    CHECK(multi(m).find("\"ac_in_w\":null,\"ac_out_w\":450") != std::string::npos);

    // NaN battery voltage would print as "nan" and break JSON.parse.
    m.battV = NAN;
    std::string s = multi(m);
    CHECK(s.find("nan") == std::string::npos);
    CHECK(s.find("\"batt_v\":0.00") != std::string::npos);
}

TEST_CASE("wsFmtAc carries reachability separately from frame validity", "[ws_frames]") {
    OpenAirData d{};
    d.probe1C = 24.4f; d.probe2C = 8.75f;
    d.blowerSpeedPct = 60; d.compressorSpeedRpm = 2400; d.errors = 0;
    d.valid = true;

    CHECK(ac(d, true, false) ==
          "{\"command\":\"ac\",\"valid\":true,\"conn\":true,"
          "\"probe1\":24.4,\"probe2\":8.8,"
          "\"blower_pct\":60,\"comp_rpm\":2400,\"errors\":0,\"needpair\":false}");

    // A cached frame stays valid after the unit drops off: valid and conn are
    // independent, which is what lets the UI strike through the snowflake while
    // still showing the last readings.
    std::string dropped = ac(d, false, false);
    CHECK(dropped.find("\"valid\":true,\"conn\":false") != std::string::npos);

    CHECK(ac(d, false, true).find("\"needpair\":true") != std::string::npos);
}

TEST_CASE("envelope helpers prefix ids with a slash", "[ws_frames]") {
    char b[WS_FRAME_BUF];

    wsFmtStatus(b, sizeof(b), "room_temp", "21.5");
    CHECK(std::string(b) ==
          "{\"command\":\"status\",\"id\":\"/room_temp\",\"value\":\"21.5\"}");

    wsFmtSetting(b, sizeof(b), "boiler", "eco");
    CHECK(std::string(b) ==
          "{\"command\":\"setting\",\"id\":\"/boiler\",\"value\":\"eco\"}");

    // Icon ids are bare — no slash (they are not settings paths).
    wsFmtIcon(b, sizeof(b), "ble", 2);
    CHECK(std::string(b) == "{\"command\":\"icon\",\"id\":\"ble\",\"state\":2}");
}

TEST_CASE("formatters truncate rather than overflow a short buffer", "[ws_frames]") {
    VictronData v{}; v.valid = true;
    char small[16];
    memset(small, 0xAA, sizeof(small));
    wsFmtSolar(small, sizeof(small), v);
    CHECK(strlen(small) < sizeof(small));          // NUL-terminated within bounds
    CHECK((unsigned char)small[sizeof(small) - 1] != 0xAA);  // did not run past
}
