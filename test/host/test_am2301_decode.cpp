#include "catch_amalgamated.hpp"
#include "am2301_codec.hpp"   // real production AM2301 decoder
#include <cmath>

// Build the data symbols for the 5 raw bytes: each bit is a low (~50 µs)
// followed by a high (26 µs = 0, 70 µs = 1), matching the sensor's encoding.
static void encodeBits(uint8_t* raw, Am2301Symbol* syms, int& symCount) {
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

        Am2301Symbol syms[48];
        int symCount;
        encodeBits(raw, syms, symCount);

        float tempC, humidity;
        bool ok = am2301Decode(syms, symCount, tempC, humidity);

        REQUIRE(ok);
        REQUIRE(std::abs(tempC - 25.3f) < 0.1f);
        REQUIRE(std::abs(humidity - 60.5f) < 0.1f);
    }

    SECTION("Negative temperature -10.5°C") {
        uint8_t raw[5];
        raw[0] = 0x01; raw[1] = 0xF4;
        raw[2] = 0x80; raw[3] = 0x69;
        raw[4] = raw[0] + raw[1] + raw[2] + raw[3];

        Am2301Symbol syms[48];
        int symCount;
        encodeBits(raw, syms, symCount);

        float tempC, humidity;
        bool ok = am2301Decode(syms, symCount, tempC, humidity);

        REQUIRE(ok);
        REQUIRE(std::abs(tempC - (-10.5f)) < 0.1f);
        REQUIRE(std::abs(humidity - 50.0f) < 0.1f);
    }

    SECTION("Zero temperature and humidity") {
        uint8_t raw[5] = {0x00, 0x00, 0x00, 0x00, 0x00};

        Am2301Symbol syms[48];
        int symCount;
        encodeBits(raw, syms, symCount);

        float tempC, humidity;
        bool ok = am2301Decode(syms, symCount, tempC, humidity);

        REQUIRE(ok);
        REQUIRE(std::abs(tempC) < 0.1f);
        REQUIRE(std::abs(humidity) < 0.1f);
    }

    SECTION("Maximum values") {
        uint8_t raw[5];
        raw[0] = 0x03; raw[1] = 0xE7;
        raw[2] = 0x7F; raw[3] = 0xFF;
        raw[4] = raw[0] + raw[1] + raw[2] + raw[3];

        Am2301Symbol syms[48];
        int symCount;
        encodeBits(raw, syms, symCount);

        float tempC, humidity;
        bool ok = am2301Decode(syms, symCount, tempC, humidity);

        REQUIRE(ok);
        REQUIRE(humidity > 99.0f);
        REQUIRE(tempC > 100.0f);
    }
}

TEST_CASE("AM2301 decode invalid data", "[am2301]") {
    SECTION("Checksum mismatch") {
        uint8_t raw[5] = {0x02, 0x5D, 0x00, 0xFD, 0xFF};

        Am2301Symbol syms[48];
        int symCount;
        encodeBits(raw, syms, symCount);

        float tempC, humidity;
        bool ok = am2301Decode(syms, symCount, tempC, humidity);

        REQUIRE_FALSE(ok);
    }

    SECTION("Too few symbols") {
        Am2301Symbol syms[20];
        for (int i = 0; i < 20; i++) {
            syms[i].level0 = 0;
            syms[i].duration0 = 50;
            syms[i].level1 = 1;
            syms[i].duration1 = 26;
        }

        float tempC, humidity;
        bool ok = am2301Decode(syms, 20, tempC, humidity);

        REQUIRE_FALSE(ok);
    }

    SECTION("All low symbols (no high pulses)") {
        Am2301Symbol syms[48];
        for (int i = 0; i < 48; i++) {
            syms[i].level0 = 0;
            syms[i].duration0 = 50;
            syms[i].level1 = 0;
            syms[i].duration1 = 50;
        }

        float tempC, humidity;
        bool ok = am2301Decode(syms, 48, tempC, humidity);

        REQUIRE_FALSE(ok);
    }
}

TEST_CASE("AM2301 decode with leading handshake", "[am2301]") {
    SECTION("Extra leading symbols are skipped") {
        uint8_t raw[5];
        raw[0] = 0x02; raw[1] = 0x5D;
        raw[2] = 0x00; raw[3] = 0xFD;
        raw[4] = raw[0] + raw[1] + raw[2] + raw[3];

        Am2301Symbol syms[48];
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
        bool ok = am2301Decode(syms, symCount, tempC, humidity);

        REQUIRE(ok);
        REQUIRE(std::abs(tempC - 25.3f) < 0.1f);
        REQUIRE(std::abs(humidity - 60.5f) < 0.1f);
    }
}
