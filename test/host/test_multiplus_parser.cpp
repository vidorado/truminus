#include "catch_amalgamated.hpp"
#include "multiplus_codec.hpp"   // real production BitReader + parser
#include <cstdint>
#include <cmath>

// Thin wrapper to keep the existing call sites readable.
static MultiplusData parseMultiplusPlaintext(const uint8_t* pt) {
    MultiplusData d = {};
    multiplusParseRecord(pt, d);
    return d;
}

TEST_CASE("Multiplus parser - normal values", "[multiplus][parser]") {
    SECTION("Inverting state with typical values") {
        uint8_t pt[16] = {};
        BitReader w(pt);
        
        pt[0] = 9;
        pt[1] = 0;
        
        int16_t rawA = -327;
        pt[2] = rawA & 0xFF;
        pt[3] = (rawA >> 8) & 0xFF;
        
        uint16_t rawV = 1286;
        pt[4] = rawV & 0xFF;
        pt[5] = (rawV >> 8) & 0x3F;
        
        uint8_t ai = 0;
        pt[5] |= (ai << 6);
        
        int32_t rawInW = 0;
        pt[6] = rawInW & 0xFF;
        pt[7] = (rawInW >> 8) & 0xFF;
        pt[8] = (rawInW >> 16) & 0x07;
        
        int32_t rawOutW = 420;
        int outBitPos = 67;
        for (int i = 0; i < 19; i++) {
            int byteIdx = (outBitPos + i) / 8;
            int bitIdx = (outBitPos + i) % 8;
            if ((rawOutW >> i) & 1) {
                pt[byteIdx] |= (1 << bitIdx);
            }
        }
        
        MultiplusData d = parseMultiplusPlaintext(pt);
        
        REQUIRE(d.deviceState == 9);
        REQUIRE(d.error == 0);
        REQUIRE(std::abs(d.battA - (-32.7f)) < 0.1f);
        REQUIRE(std::abs(d.battV - 12.86f) < 0.01f);
        REQUIRE(d.acInW == 0);
        REQUIRE(d.acOutW == 420);
        REQUIRE(d.valid);
    }
}

TEST_CASE("Multiplus parser - sentinel values", "[multiplus][parser]") {
    SECTION("All sentinels") {
        uint8_t pt[16] = {};
        
        pt[0] = 0xFF;
        pt[1] = 0xFF;
        
        int16_t rawA = 0x7FFF;
        pt[2] = rawA & 0xFF;
        pt[3] = (rawA >> 8) & 0xFF;
        
        uint16_t rawV = 0x3FFF;
        pt[4] = rawV & 0xFF;
        pt[5] = (rawV >> 8) & 0x3F;
        
        uint8_t ai = 3;
        pt[5] |= (ai << 6);
        
        int32_t rawInW = 0x3FFFF;
        pt[6] = rawInW & 0xFF;
        pt[7] = (rawInW >> 8) & 0xFF;
        pt[8] = (rawInW >> 16) & 0x07;
        
        int32_t rawOutW = 0x3FFFF;
        int outBitPos = 67;
        for (int i = 0; i < 19; i++) {
            int byteIdx = (outBitPos + i) / 8;
            int bitIdx = (outBitPos + i) % 8;
            if ((rawOutW >> i) & 1) {
                pt[byteIdx] |= (1 << bitIdx);
            }
        }
        
        uint8_t al = 3;
        int alarmBitPos = 86;
        for (int i = 0; i < 2; i++) {
            int byteIdx = (alarmBitPos + i) / 8;
            int bitIdx = (alarmBitPos + i) % 8;
            if ((al >> i) & 1) {
                pt[byteIdx] |= (1 << bitIdx);
            }
        }
        
        uint8_t rawT = 0x7F;
        int tempBitPos = 88;
        for (int i = 0; i < 7; i++) {
            int byteIdx = (tempBitPos + i) / 8;
            int bitIdx = (tempBitPos + i) % 8;
            if ((rawT >> i) & 1) {
                pt[byteIdx] |= (1 << bitIdx);
            }
        }
        
        uint8_t socR = 0x7F;
        int socBitPos = 95;
        for (int i = 0; i < 7; i++) {
            int byteIdx = (socBitPos + i) / 8;
            int bitIdx = (socBitPos + i) % 8;
            if ((socR >> i) & 1) {
                pt[byteIdx] |= (1 << bitIdx);
            }
        }
        
        MultiplusData d = parseMultiplusPlaintext(pt);
        
        REQUIRE(d.deviceState == 0xFF);
        REQUIRE(d.error == 0xFF);
        REQUIRE(d.battA == 0.0f);
        REQUIRE(std::isnan(d.battV));
        REQUIRE(d.acInState == 3);
        REQUIRE(d.acInW == MULTI_POWER_NA);
        REQUIRE(d.acOutW == MULTI_POWER_NA);
        REQUIRE(d.alarm == 3);
        REQUIRE(d.battTempC == -128);
        REQUIRE(d.soc == 0xFF);
        REQUIRE(d.valid);
    }
}

TEST_CASE("Multiplus parser - temperature and SOC", "[multiplus][parser]") {
    SECTION("Temperature with offset") {
        uint8_t pt[16] = {};
        
        uint8_t rawT = 65;
        int tempBitPos = 88;
        for (int i = 0; i < 7; i++) {
            int byteIdx = (tempBitPos + i) / 8;
            int bitIdx = (tempBitPos + i) % 8;
            if ((rawT >> i) & 1) {
                pt[byteIdx] |= (1 << bitIdx);
            }
        }
        
        MultiplusData d = parseMultiplusPlaintext(pt);
        REQUIRE(d.battTempC == 25);
    }

    SECTION("SOC value") {
        uint8_t pt[16] = {};
        
        uint8_t socR = 87;
        int socBitPos = 95;
        for (int i = 0; i < 7; i++) {
            int byteIdx = (socBitPos + i) / 8;
            int bitIdx = (socBitPos + i) % 8;
            if ((socR >> i) & 1) {
                pt[byteIdx] |= (1 << bitIdx);
            }
        }
        
        MultiplusData d = parseMultiplusPlaintext(pt);
        REQUIRE(d.soc == 87);
    }
}
