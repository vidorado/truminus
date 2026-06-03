#include "catch_amalgamated.hpp"
#include <cstdint>

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

TEST_CASE("BitReader unsigned reads", "[multiplus][bitreader]") {
    SECTION("Read 8 bits from start") {
        uint8_t data[16] = {0xAB, 0xCD, 0xEF};
        BitReader r(data);
        REQUIRE(r.readU(8) == 0xAB);
        REQUIRE(r.readU(8) == 0xCD);
        REQUIRE(r.readU(8) == 0xEF);
    }

    SECTION("Read across byte boundary") {
        uint8_t data[16] = {0b10101010, 0b11001100};
        BitReader r(data);
        REQUIRE(r.readU(4) == 0b1010);
        REQUIRE(r.readU(8) == 0b11001010);
    }

    SECTION("Read 16-bit value") {
        uint8_t data[16] = {0x34, 0x12};
        BitReader r(data);
        REQUIRE(r.readU(16) == 0x1234);
    }

    SECTION("Read across 64-bit boundary") {
        uint8_t data[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
        BitReader r(data);
        r.readU(60);
        REQUIRE(r.readU(8) == 0x1F);
    }
}

TEST_CASE("BitReader signed reads", "[multiplus][bitreader]") {
    SECTION("Positive 8-bit value") {
        uint8_t data[16] = {0x7F};
        BitReader r(data);
        REQUIRE(r.readS(8) == 127);
    }

    SECTION("Negative 8-bit value (-1)") {
        uint8_t data[16] = {0xFF};
        BitReader r(data);
        REQUIRE(r.readS(8) == -1);
    }

    SECTION("Negative 16-bit value") {
        uint8_t data[16] = {0xFF, 0xFF};
        BitReader r(data);
        REQUIRE(r.readS(16) == -1);
    }

    SECTION("19-bit signed value (VE.Bus power)") {
        uint8_t data[16] = {0xFF, 0xFF, 0x07};
        BitReader r(data);
        REQUIRE(r.readS(19) == -1);
    }

    SECTION("19-bit positive value") {
        uint8_t data[16] = {0x00, 0x01, 0x00};
        BitReader r(data);
        REQUIRE(r.readS(19) == 256);
    }
}

TEST_CASE("BitReader position tracking", "[multiplus][bitreader]") {
    uint8_t data[16] = {0};
    BitReader r(data);

    REQUIRE(r.pos == 0);
    r.readU(8);
    REQUIRE(r.pos == 8);
    r.readU(16);
    REQUIRE(r.pos == 24);
    r.readS(19);
    REQUIRE(r.pos == 43);
}
