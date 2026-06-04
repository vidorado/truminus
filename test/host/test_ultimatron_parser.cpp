#include "catch_amalgamated.hpp"
#include "ultimatron_codec.hpp"   // real production parser
#include <cstring>
#include <cmath>

// Build a 0xDD 0x03 0x00 basic-info response with the fields the parser reads.
// rawV = V*100 (BE u16), rawA = A*100 (BE s16), soc at d[23],
// temp at d[27..28] as Kelvin*10 (tempC = (rawT-2731)/10), len >= 30.
static void buildPacket(uint8_t* d, int len, uint16_t rawV, int16_t rawA,
                        uint8_t soc, uint16_t rawT) {
    memset(d, 0, len);
    d[0] = 0xdd; d[1] = 0x03; d[2] = 0x00;
    d[4] = (rawV >> 8) & 0xFF; d[5] = rawV & 0xFF;
    d[6] = (rawA >> 8) & 0xFF; d[7] = rawA & 0xFF;
    if (len > 23) d[23] = soc;
    if (len >= 30) { d[27] = (rawT >> 8) & 0xFF; d[28] = rawT & 0xFF; }
}

TEST_CASE("Ultimatron parse - full valid packet", "[ultimatron]") {
    uint8_t d[30];
    // 13.25 V, -1.50 A, 87 %, 25.0 °C → rawT = 250 + 2731 = 2981
    buildPacket(d, 30, 1325, (int16_t)-150, 87, 2981);

    UltimatronData out = {};
    REQUIRE(ultimatronParseResponse(d, 30, out));
    REQUIRE(out.valid);
    REQUIRE(std::abs(out.battV - 13.25f) < 0.001f);
    REQUIRE(std::abs(out.battA - (-1.50f)) < 0.001f);
    REQUIRE(out.soc == 87);
    REQUIRE(std::abs(out.tempC - 25.0f) < 0.05f);
}

TEST_CASE("Ultimatron parse - no temperature when short of 30 bytes", "[ultimatron]") {
    uint8_t d[27];
    buildPacket(d, 27, 1280, (int16_t)100, 50, 0);

    UltimatronData out = {};
    REQUIRE(ultimatronParseResponse(d, 27, out));
    REQUIRE(out.valid);
    REQUIRE(std::abs(out.battV - 12.80f) < 0.001f);
    REQUIRE(std::abs(out.battA - 1.00f) < 0.001f);
    REQUIRE(out.soc == 50);
    REQUIRE(out.tempC == 0.0f);   // left at default, no temp bytes
}

TEST_CASE("Ultimatron parse - rejects malformed packets", "[ultimatron]") {
    uint8_t d[30];
    buildPacket(d, 30, 1325, 0, 87, 2981);

    SECTION("too short") {
        UltimatronData out = {};
        REQUIRE_FALSE(ultimatronParseResponse(d, 20, out));
        REQUIRE_FALSE(out.valid);   // out left untouched
    }
    SECTION("wrong start byte") {
        d[0] = 0xee;
        UltimatronData out = {};
        REQUIRE_FALSE(ultimatronParseResponse(d, 30, out));
    }
    SECTION("wrong command byte") {
        d[1] = 0x04;
        UltimatronData out = {};
        REQUIRE_FALSE(ultimatronParseResponse(d, 30, out));
    }
}
