#include "catch_amalgamated.hpp"
#include "test_helpers.hpp"
#include <cmath>

struct RmtSymbol {
    uint16_t duration0;
    uint16_t level0;
    uint16_t duration1;
    uint16_t level1;
};

static bool decode(const RmtSymbol* syms, size_t n, float& tempC, float& humidity) {
    constexpr int AM2301_BIT_THRESH_US = 50;
    constexpr int AM2301_MAX_SYMBOLS = 48;
    
    uint8_t bits[AM2301_MAX_SYMBOLS];
    int nbits = 0;

    for (size_t i = 0; i < n && nbits < AM2301_MAX_SYMBOLS; i++) {
        uint16_t hi_us = 0;
        if (syms[i].level0 == 1)       hi_us = syms[i].duration0;
        else if (syms[i].level1 == 1)  hi_us = syms[i].duration1;
        else                           continue;
        if (hi_us == 0) continue;
        bits[nbits++] = (hi_us > AM2301_BIT_THRESH_US) ? 1 : 0;
    }

    if (nbits < 40) return false;
    
    const uint8_t* b = bits + (nbits - 40);

    uint8_t raw[5] = {0};
    for (int i = 0; i < 40; i++)
        raw[i / 8] = (raw[i / 8] << 1) | b[i];

    uint8_t sum = raw[0] + raw[1] + raw[2] + raw[3];
    if (sum != raw[4]) return false;

    humidity = ((raw[0] << 8) | raw[1]) / 10.0f;
    int16_t t = ((raw[2] & 0x7F) << 8) | raw[3];
    tempC = (raw[2] & 0x80) ? -t / 10.0f : t / 10.0f;
    return true;
}

static void encodeBits(uint8_t* raw, RmtSymbol* syms, int& symCount) {
    symCount = 0;
    for (int byte = 0; byte < 5; byte++) {
        for (int bit = 7; bit >= 0; bit--) {
            bool isOne = (raw[byte] >> bit) & 1;
            syms[symCount].level0 = 0;
            syms[symCount].duration0 = 50;
            syms[symCount].level1 = 1;
            syms[symCount].duration1 = isOne ? 70 : 26;
            symCount++;
        }
    }
}

TEST_CASE("AM2301 decode valid data", "[am2301]") {
    SECTION("Temperature 25.3°C, humidity 60.5%") {
        uint8_t raw[5];
        raw[0] = 0x02; raw[1] = 0x5D;
        raw[2] = 0x00; raw[3] = 0xFD;
        raw[4] = raw[0] + raw[1] + raw[2] + raw[3];
        
        RmtSymbol syms[48];
        int symCount;
        encodeBits(raw, syms, symCount);
        
        float tempC, humidity;
        bool ok = decode(syms, symCount, tempC, humidity);
        
        REQUIRE(ok);
        REQUIRE(std::abs(tempC - 25.3f) < 0.1f);
        REQUIRE(std::abs(humidity - 60.5f) < 0.1f);
    }

    SECTION("Negative temperature -10.5°C") {
        uint8_t raw[5];
        raw[0] = 0x01; raw[1] = 0xF4;
        raw[2] = 0x80; raw[3] = 0x69;
        raw[4] = raw[0] + raw[1] + raw[2] + raw[3];
        
        RmtSymbol syms[48];
        int symCount;
        encodeBits(raw, syms, symCount);
        
        float tempC, humidity;
        bool ok = decode(syms, symCount, tempC, humidity);
        
        REQUIRE(ok);
        REQUIRE(std::abs(tempC - (-10.5f)) < 0.1f);
        REQUIRE(std::abs(humidity - 50.0f) < 0.1f);
    }

    SECTION("Zero temperature and humidity") {
        uint8_t raw[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
        
        RmtSymbol syms[48];
        int symCount;
        encodeBits(raw, syms, symCount);
        
        float tempC, humidity;
        bool ok = decode(syms, symCount, tempC, humidity);
        
        REQUIRE(ok);
        REQUIRE(std::abs(tempC) < 0.1f);
        REQUIRE(std::abs(humidity) < 0.1f);
    }

    SECTION("Maximum values") {
        uint8_t raw[5];
        raw[0] = 0x03; raw[1] = 0xE7;
        raw[2] = 0x7F; raw[3] = 0xFF;
        raw[4] = raw[0] + raw[1] + raw[2] + raw[3];
        
        RmtSymbol syms[48];
        int symCount;
        encodeBits(raw, syms, symCount);
        
        float tempC, humidity;
        bool ok = decode(syms, symCount, tempC, humidity);
        
        REQUIRE(ok);
        REQUIRE(humidity > 99.0f);
        REQUIRE(tempC > 100.0f);
    }
}

TEST_CASE("AM2301 decode invalid data", "[am2301]") {
    SECTION("Checksum mismatch") {
        uint8_t raw[5] = {0x02, 0x5D, 0x00, 0xFD, 0xFF};
        
        RmtSymbol syms[48];
        int symCount;
        encodeBits(raw, syms, symCount);
        
        float tempC, humidity;
        bool ok = decode(syms, symCount, tempC, humidity);
        
        REQUIRE_FALSE(ok);
    }

    SECTION("Too few symbols") {
        RmtSymbol syms[20];
        for (int i = 0; i < 20; i++) {
            syms[i].level0 = 0;
            syms[i].duration0 = 50;
            syms[i].level1 = 1;
            syms[i].duration1 = 26;
        }
        
        float tempC, humidity;
        bool ok = decode(syms, 20, tempC, humidity);
        
        REQUIRE_FALSE(ok);
    }

    SECTION("All low symbols (no high pulses)") {
        RmtSymbol syms[48];
        for (int i = 0; i < 48; i++) {
            syms[i].level0 = 0;
            syms[i].duration0 = 50;
            syms[i].level1 = 0;
            syms[i].duration1 = 50;
        }
        
        float tempC, humidity;
        bool ok = decode(syms, 48, tempC, humidity);
        
        REQUIRE_FALSE(ok);
    }
}

TEST_CASE("AM2301 decode with leading handshake", "[am2301]") {
    SECTION("Extra leading symbols are skipped") {
        uint8_t raw[5];
        raw[0] = 0x02; raw[1] = 0x5D;
        raw[2] = 0x00; raw[3] = 0xFD;
        raw[4] = raw[0] + raw[1] + raw[2] + raw[3];
        
        RmtSymbol syms[48];
        int symCount = 0;
        
        syms[symCount].level0 = 0;
        syms[symCount].duration0 = 80;
        syms[symCount].level1 = 1;
        syms[symCount].duration1 = 80;
        symCount++;
        
        syms[symCount].level0 = 0;
        syms[symCount].duration0 = 50;
        syms[symCount].level1 = 1;
        syms[symCount].duration1 = 80;
        symCount++;
        
        int dataSymCount;
        encodeBits(raw, syms + symCount, dataSymCount);
        symCount += dataSymCount;
        
        float tempC, humidity;
        bool ok = decode(syms, symCount, tempC, humidity);
        
        REQUIRE(ok);
        REQUIRE(std::abs(tempC - 25.3f) < 0.1f);
        REQUIRE(std::abs(humidity - 60.5f) < 0.1f);
    }
}
