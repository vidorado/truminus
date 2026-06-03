#include "catch_amalgamated.hpp"
#include "mode_controller.hpp"   // real production derive_mode + P4ControlState

// These tests exercise the shipping derive_mode() from main/mode_controller.cpp,
// not a local copy, so any drift in the real implementation is caught here.

TEST_CASE("derive_mode: heating off, fan off", "[derive_mode]") {
    P4ControlState cs = {false, 0, 0, 0, 20.0f};
    auto r = derive_mode(cs);
    REQUIRE(r.pumpOrFan == 0x10);
    REQUIRE(r.roomSp == 0.0);
    REQUIRE(r.waterSp == 0.0);
}

TEST_CASE("derive_mode: heating off, fan eco", "[derive_mode]") {
    P4ControlState cs = {false, 1, 0, 0, 20.0f};
    auto r = derive_mode(cs);
    REQUIRE(r.pumpOrFan == 0x11);
    REQUIRE(r.roomSp == 0.0);
    REQUIRE(r.waterSp == 0.0);
}

TEST_CASE("derive_mode: heating off, fan high", "[derive_mode]") {
    P4ControlState cs = {false, 2, 0, 0, 20.0f};
    auto r = derive_mode(cs);
    REQUIRE(r.pumpOrFan == 0x12);
    REQUIRE(r.roomSp == 0.0);
    REQUIRE(r.waterSp == 0.0);
}

TEST_CASE("derive_mode: heating off, fan level 5", "[derive_mode]") {
    P4ControlState cs = {false, 7, 0, 0, 20.0f};  // fanMode 7 = level 5
    auto r = derive_mode(cs);
    REQUIRE(r.pumpOrFan == 0x15);  // 0x10 | 5
    REQUIRE(r.roomSp == 0.0);
    REQUIRE(r.waterSp == 0.0);
}

TEST_CASE("derive_mode: heating off, fan level 10", "[derive_mode]") {
    P4ControlState cs = {false, 12, 0, 0, 20.0f};  // fanMode 12 = level 10
    auto r = derive_mode(cs);
    REQUIRE(r.pumpOrFan == 0x1A);  // 0x10 | 10
    REQUIRE(r.roomSp == 0.0);
    REQUIRE(r.waterSp == 0.0);
}

TEST_CASE("derive_mode: heating on, fan eco", "[derive_mode]") {
    P4ControlState cs = {true, 1, 0, 0, 22.5f};
    auto r = derive_mode(cs);
    REQUIRE(r.pumpOrFan == 1);
    REQUIRE(r.roomSp == 22.5);
    REQUIRE(r.waterSp == 0.0);
}

TEST_CASE("derive_mode: heating on, fan high", "[derive_mode]") {
    P4ControlState cs = {true, 2, 0, 0, 22.5f};
    auto r = derive_mode(cs);
    REQUIRE(r.pumpOrFan == 2);
    REQUIRE(r.roomSp == 22.5);
    REQUIRE(r.waterSp == 0.0);
}

TEST_CASE("derive_mode: heating on, fan level 5 forces eco", "[derive_mode]") {
    P4ControlState cs = {true, 7, 0, 0, 22.5f};  // fanMode 7 = level 5
    auto r = derive_mode(cs);
    // Heating on + numeric fan level: forced to eco (1)
    REQUIRE(r.pumpOrFan == 1);
    REQUIRE(r.roomSp == 22.5);
    REQUIRE(r.waterSp == 0.0);
}

TEST_CASE("derive_mode: heating on, fan off forces eco", "[derive_mode]") {
    P4ControlState cs = {true, 0, 0, 0, 22.5f};
    auto r = derive_mode(cs);
    // Heating on + fan off: forced to eco (1)
    REQUIRE(r.pumpOrFan == 1);
    REQUIRE(r.roomSp == 22.5);
    REQUIRE(r.waterSp == 0.0);
}

TEST_CASE("derive_mode: boiler off", "[derive_mode]") {
    P4ControlState cs = {false, 0, 0, 0, 20.0f};
    auto r = derive_mode(cs);
    REQUIRE(r.waterSp == 0.0);
}

TEST_CASE("derive_mode: boiler eco (40°C)", "[derive_mode]") {
    P4ControlState cs = {false, 0, 1, 0, 20.0f};
    auto r = derive_mode(cs);
    REQUIRE(r.waterSp == 40.0);
}

TEST_CASE("derive_mode: boiler high (60°C)", "[derive_mode]") {
    P4ControlState cs = {false, 0, 2, 0, 20.0f};
    auto r = derive_mode(cs);
    REQUIRE(r.waterSp == 60.0);
}

TEST_CASE("derive_mode: boiler boost (60°C)", "[derive_mode]") {
    P4ControlState cs = {false, 0, 3, 0, 20.0f};
    auto r = derive_mode(cs);
    // Boost mode uses 60°C (no waterboost cycle in MVP)
    REQUIRE(r.waterSp == 60.0);
}

TEST_CASE("derive_mode: combined heating + fan high + boiler eco", "[derive_mode]") {
    P4ControlState cs = {true, 2, 1, 0, 23.0f};
    auto r = derive_mode(cs);
    REQUIRE(r.pumpOrFan == 2);
    REQUIRE(r.roomSp == 23.0);
    REQUIRE(r.waterSp == 40.0);
}

TEST_CASE("derive_mode: combined fan-only level 7 + boiler boost", "[derive_mode]") {
    P4ControlState cs = {false, 9, 3, 0, 20.0f};  // fanMode 9 = level 7
    auto r = derive_mode(cs);
    REQUIRE(r.pumpOrFan == 0x17);  // 0x10 | 7
    REQUIRE(r.roomSp == 0.0);
    REQUIRE(r.waterSp == 60.0);
}
