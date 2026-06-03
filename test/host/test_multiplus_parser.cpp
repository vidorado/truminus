#include "catch_amalgamated.hpp"
#include <cstdint>
#include <cmath>

struct BitReader {
    uint64_t lo;
    uint64_t hi;
    int pos;

    BitReader(const uint8_t* p) : pos(0) {
        lo = 0; hi = 0;
        for (int i = 0; i < 8; i++) lo |= ((uint64_t)p[i]) << (i * 8);
        for (int i = 0; i < 8; i++) hi |= ((uint64_t)p[8 + i]) << (i * 8);
    }

    uint64_t readU(int bits) {
        uint64_t v;
        if (pos + bits <= 64) {
            v = (lo >> pos) & ((bits < 64) ? ((1ULL << bits) - 1) : ~0ULL);
        } else if (pos >= 64) {
            int p = pos - 64;
            v = (hi >> p) & ((bits < 64) ? ((1ULL << bits) - 1) : ~0ULL);
        } else {
            int loBits = 64 - pos;
            int hiBits = bits - loBits;
            uint64_t loPart = lo >> pos;
            uint64_t hiPart = hi & ((1ULL << hiBits) - 1);
            v = loPart | (hiPart << loBits);
        }
        pos += bits;
        return v;
    }

    int64_t readS(int bits) {
        uint64_t v = readU(bits);
        if (bits < 64) {
            uint64_t signbit = 1ULL << (bits - 1);
            if (v & signbit) {
                v |= ~((1ULL << bits) - 1);
            }
        }
        return (int64_t)v;
    }
};

struct MultiplusData {
    uint8_t  deviceState;
    uint8_t  error;
    uint8_t  acInState;
    uint8_t  alarm;
    int32_t  acInW;
    int32_t  acOutW;
    float    battA;
    float    battV;
    int8_t   battTempC;
    uint8_t  soc;
    bool     valid;
};

static constexpr int32_t MULTI_POWER_NA = -999999;

static MultiplusData parseMultiplusPlaintext(const uint8_t* pt) {
    BitReader r(pt);
    uint8_t  ds  = (uint8_t)r.readU(8);
    uint8_t  er  = (uint8_t)r.readU(8);
    int16_t  rawA = (int16_t)r.readS(16);
    uint16_t rawV = (uint16_t)r.readU(14);
    uint8_t  ai  = (uint8_t)r.readU(2);
    int32_t  rawInW  = (int32_t)r.readS(19);
    int32_t  rawOutW = (int32_t)r.readS(19);
    uint8_t  al  = (uint8_t)r.readU(2);
    uint8_t  rawT = (uint8_t)r.readU(7);
    uint8_t  socR = (uint8_t)r.readU(7);

    MultiplusData d = {};
    d.deviceState = ds;
    d.error       = er;
    d.acInState   = ai;
    d.alarm       = al;
    d.acInW       = (rawInW  == 0x3FFFF) ? MULTI_POWER_NA : rawInW;
    d.acOutW      = (rawOutW == 0x3FFFF) ? MULTI_POWER_NA : rawOutW;
    d.battA       = (rawA == 0x7FFF)  ? 0.0f           : rawA * 0.1f;
    d.battV       = (rawV == 0x3FFF)  ? NAN            : rawV * 0.01f;
    d.battTempC   = (rawT == 0x7F)    ? (int8_t)-128   : (int8_t)((int)rawT - 40);
    d.soc         = (socR == 0x7F)    ? 0xFF           : socR;
    d.valid       = true;

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
