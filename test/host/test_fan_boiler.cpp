#include "catch_amalgamated.hpp"
#include "mode_controller.hpp"   // real production conversions
#include <cstring>

TEST_CASE("Fan string to int conversion", "[fan]") {
    SECTION("Valid strings") {
        REQUIRE(fanStrToInt("off") == 0);
        REQUIRE(fanStrToInt("eco") == 1);
        REQUIRE(fanStrToInt("high") == 2);
        REQUIRE(fanStrToInt("1") == 3);
        REQUIRE(fanStrToInt("5") == 7);
        REQUIRE(fanStrToInt("10") == 12);
    }

    SECTION("Invalid strings") {
        REQUIRE(fanStrToInt(nullptr) == -1);
        REQUIRE(fanStrToInt("") == -1);
        REQUIRE(fanStrToInt("invalid") == -1);
        REQUIRE(fanStrToInt("0") == -1);
        REQUIRE(fanStrToInt("11") == -1);
        REQUIRE(fanStrToInt("-1") == -1);
    }
}

TEST_CASE("Fan int to string conversion", "[fan]") {
    SECTION("Valid modes") {
        REQUIRE(strcmp(fanIntToStr(0), "off") == 0);
        REQUIRE(strcmp(fanIntToStr(1), "eco") == 0);
        REQUIRE(strcmp(fanIntToStr(2), "high") == 0);
        REQUIRE(strcmp(fanIntToStr(3), "1") == 0);
        REQUIRE(strcmp(fanIntToStr(7), "5") == 0);
        REQUIRE(strcmp(fanIntToStr(12), "10") == 0);
    }

    SECTION("Invalid modes default to off") {
        REQUIRE(strcmp(fanIntToStr(-1), "off") == 0);
        REQUIRE(strcmp(fanIntToStr(13), "off") == 0);
        REQUIRE(strcmp(fanIntToStr(100), "off") == 0);
    }
}

TEST_CASE("Boiler string to int conversion", "[boiler]") {
    SECTION("Valid strings") {
        REQUIRE(boilerStrToInt("off") == 0);
        REQUIRE(boilerStrToInt("eco") == 1);
        REQUIRE(boilerStrToInt("high") == 2);
        REQUIRE(boilerStrToInt("boost") == 3);
    }

    SECTION("Invalid strings") {
        REQUIRE(boilerStrToInt(nullptr) == -1);
        REQUIRE(boilerStrToInt("") == -1);
        REQUIRE(boilerStrToInt("invalid") == -1);
        REQUIRE(boilerStrToInt("0") == -1);
    }
}

TEST_CASE("Boiler int to string conversion", "[boiler]") {
    SECTION("Valid modes") {
        REQUIRE(strcmp(boilerIntToStr(0), "off") == 0);
        REQUIRE(strcmp(boilerIntToStr(1), "eco") == 0);
        REQUIRE(strcmp(boilerIntToStr(2), "high") == 0);
        REQUIRE(strcmp(boilerIntToStr(3), "boost") == 0);
    }

    SECTION("Invalid modes default to off") {
        REQUIRE(strcmp(boilerIntToStr(-1), "off") == 0);
        REQUIRE(strcmp(boilerIntToStr(4), "off") == 0);
        REQUIRE(strcmp(boilerIntToStr(100), "off") == 0);
    }
}
