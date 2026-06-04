#include "catch_amalgamated.hpp"
#include "victron_codec.hpp"   // real production parser
#include <cstring>
#include <cmath>

// Build a decrypted SolarCharger plaintext record. Layout (little-endian):
//   [0] state  [1] errCode  [2..3] V*100 (s16)  [4..5] A*10 (s16)
//   [6..7] kWh*100 (u16)    [8..9] PV W (u16)
static void buildPlain(uint8_t pt[16], uint8_t state, uint8_t err,
                       int16_t rawV, int16_t rawA, uint16_t rawKwh, uint16_t rawPv) {
    memset(pt, 0, 16);
    pt[0] = state; pt[1] = err;
    pt[2] = rawV & 0xFF;   pt[3] = (rawV >> 8) & 0xFF;
    pt[4] = rawA & 0xFF;   pt[5] = (rawA >> 8) & 0xFF;
    pt[6] = rawKwh & 0xFF; pt[7] = (rawKwh >> 8) & 0xFF;
    pt[8] = rawPv & 0xFF;  pt[9] = (rawPv >> 8) & 0xFF;
}

TEST_CASE("Victron parse - valid record", "[victron]") {
    uint8_t pt[16];
    // state 4 (absorption), 13.20 V, 2.5 A, 1.50 kWh, 120 W
    buildPlain(pt, 4, 0, 1320, 25, 150, 120);

    VictronData out = {};
    REQUIRE(victronParseRecord(pt, out));
    REQUIRE(out.valid);
    REQUIRE(out.state == 4);
    REQUIRE(out.errCode == 0);
    REQUIRE(std::abs(out.battV - 13.20f) < 0.001f);
    REQUIRE(std::abs(out.battA - 2.5f) < 0.001f);
    REQUIRE(std::abs(out.kWhToday - 1.50f) < 0.001f);
    REQUIRE(std::abs(out.pvW - 120.0f) < 0.001f);
}

TEST_CASE("Victron parse - current 'not available' sentinel", "[victron]") {
    uint8_t pt[16];
    buildPlain(pt, 5, 0, 1280, (int16_t)0x7FFF, 0, 0);

    VictronData out = {};
    REQUIRE(victronParseRecord(pt, out));
    REQUIRE(out.battA == 0.0f);          // sentinel maps to 0, record still valid
    REQUIRE(std::abs(out.battV - 12.80f) < 0.001f);
}

TEST_CASE("Victron parse - voltage sentinel rejects the record", "[victron]") {
    uint8_t pt[16];
    buildPlain(pt, 3, 0, (int16_t)0x7FFF, 25, 150, 120);

    VictronData out = {};
    REQUIRE_FALSE(victronParseRecord(pt, out));
    REQUIRE_FALSE(out.valid);            // out left untouched
}

TEST_CASE("Victron parse - negative current (discharging)", "[victron]") {
    uint8_t pt[16];
    buildPlain(pt, 5, 0, 1300, (int16_t)-15, 0, 0);  // -1.5 A

    VictronData out = {};
    REQUIRE(victronParseRecord(pt, out));
    REQUIRE(std::abs(out.battA - (-1.5f)) < 0.001f);
}
