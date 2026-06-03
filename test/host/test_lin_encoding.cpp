#include "catch_amalgamated.hpp"
#include "lin_codec.hpp"   // real production LIN value codec
#include <cmath>

using Catch::Approx;

TEST_CASE("Temperature encoding to Kelvin x10", "[lin][encoding]") {
    uint8_t buf[2];

    SECTION("0°C encodes to 2730 (0xAA 0x0A)") {
        encodeTempKelvinX10(0.0, buf);
        REQUIRE(buf[0] == 0xAA);
        REQUIRE(buf[1] == 0x0A);
    }

    SECTION("20°C encodes to 2930 (0x72 0x0B)") {
        encodeTempKelvinX10(20.0, buf);
        REQUIRE(buf[0] == 0x72);
        REQUIRE(buf[1] == 0x0B);
    }

    SECTION("-273°C (absolute zero) encodes to 0") {
        encodeTempKelvinX10(-273.0, buf);
        REQUIRE(buf[0] == 0x00);
        REQUIRE(buf[1] == 0x00);
    }

    SECTION("25.5°C encodes to 2985 (0xA9 0x0B)") {
        encodeTempKelvinX10(25.5, buf);
        REQUIRE(buf[0] == 0xA9);
        REQUIRE(buf[1] == 0x0B);
    }
}

TEST_CASE("Frame 0x21 room temperature parsing", "[lin][parsing]") {
    SECTION("20°C (raw 2930)") {
        uint8_t data[] = {0x72, 0x0B, 0x00};
        double temp = parseF21RoomTemp(data);
        REQUIRE(!std::isnan(temp));
        REQUIRE(temp == Approx(20.0).margin(0.1));
    }

    SECTION("0°C (raw 2730)") {
        uint8_t data[] = {0xAA, 0x0A, 0x00};
        double temp = parseF21RoomTemp(data);
        REQUIRE(!std::isnan(temp));
        REQUIRE(temp == Approx(0.0).margin(0.1));
    }

    SECTION("0°C (raw 2730)") {
        uint8_t data[] = {0xAA, 0x0A, 0x00};
        double temp = parseF21RoomTemp(data);
        REQUIRE(!std::isnan(temp));
        REQUIRE(temp == Approx(0.0).margin(0.1));
    }

    SECTION("Out of range (>50°C) returns NaN") {
        uint8_t data[] = {0xFF, 0xFF, 0x00};
        double temp = parseF21RoomTemp(data);
        REQUIRE(std::isnan(temp));
    }

    SECTION("Out of range (<0°C) returns NaN") {
        uint8_t data[] = {0x00, 0x00, 0x00};
        double temp = parseF21RoomTemp(data);
        REQUIRE(std::isnan(temp));
    }
}

TEST_CASE("Frame 0x21 water temperature parsing", "[lin][parsing]") {
    SECTION("40°C (raw 3130)") {
        uint8_t data[] = {0x00, 0xA0, 0xC3};
        double temp = parseF21WaterTemp(data);
        REQUIRE(!std::isnan(temp));
        REQUIRE(temp == Approx(40.0).margin(0.1));
    }

    SECTION("60°C (raw 3330)") {
        uint8_t data[] = {0x00, 0x20, 0xD0};
        double temp = parseF21WaterTemp(data);
        REQUIRE(!std::isnan(temp));
        REQUIRE(temp == Approx(60.0).margin(0.1));
    }

    SECTION("60°C (raw 3330)") {
        uint8_t data[] = {0x00, 0x20, 0xD0};
        double temp = parseF21WaterTemp(data);
        REQUIRE(!std::isnan(temp));
        REQUIRE(temp == Approx(60.0).margin(0.1));
    }

    SECTION("Out of range (>100°C) returns NaN") {
        uint8_t data[] = {0x00, 0xFF, 0xFF};
        double temp = parseF21WaterTemp(data);
        REQUIRE(std::isnan(temp));
    }
}

TEST_CASE("Frame 0x22 water heating flag", "[lin][parsing]") {
    SECTION("Heating active (0x40 or 0x50 in byte 1)") {
        uint8_t data1[] = {0x00, 0x40};
        REQUIRE(parseF22WaterHeating(data1) == true);

        uint8_t data2[] = {0x00, 0x50};
        REQUIRE(parseF22WaterHeating(data2) == true);
    }

    SECTION("Heating inactive (0x00 or 0xD0 in byte 1)") {
        uint8_t data1[] = {0x00, 0x00};
        REQUIRE(parseF22WaterHeating(data1) == false);

        uint8_t data2[] = {0x00, 0xD0};
        REQUIRE(parseF22WaterHeating(data2) == false);
    }
}
