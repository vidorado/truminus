#include "catch_amalgamated.hpp"
#include <cstring>
#include <cstdint>

// Frame 0x20 encoding functions (from main.cpp)
static void f20_setRoomSetpoint(double celsius, uint8_t* f20_data) {
    if (celsius < 5.0 || celsius > 30.0) {
        f20_data[0] = 0xAA;
        f20_data[1] = 0xAA;
        return;
    }
    uint16_t raw = (uint16_t)((celsius + 273.0) * 10.0);
    f20_data[0] = raw & 0xFF;
    f20_data[1] = 0xA0 | ((raw >> 8) & 0x0F);
}

static void f20_setWaterSetpoint(double celsius, uint8_t* f20_data) {
    if (celsius < 1.0) {
        f20_data[2] = 0xAA;
        return;
    }
    uint16_t raw = (uint16_t)((celsius + 273.0) * 10.0);
    f20_data[2] = (raw >> 4) & 0xFF;
}

static void f20_setFanAndWater(uint8_t pumpOrFan, uint8_t waterMode, uint8_t* f20_data) {
    uint8_t fanNibble;
    if (pumpOrFan == 1) {
        fanNibble = 0xB;
    } else if (pumpOrFan == 2) {
        fanNibble = 0xD;
    } else {
        uint8_t level = (pumpOrFan >= 0x10) ? (pumpOrFan & 0x0F) : pumpOrFan;
        fanNibble = level;
    }
    f20_data[5] = (fanNibble << 4) | (waterMode & 0x0F);
}

// Frame 0x05 energy selection
static void f05_setEnergySelection(int energyIdx, uint8_t* f05_data) {
    static const uint8_t priorities[5] = { 1, 3, 3, 2, 2 };
    if (energyIdx < 0 || energyIdx > 4) energyIdx = 0;
    f05_data[0] = priorities[energyIdx];
}

// Frame 0x06 power limit
static void f06_setPowerLimit(int energyIdx, uint8_t* f06_data) {
    static const uint16_t limits[5] = { 0, 900, 1800, 900, 1800 };
    if (energyIdx < 0 || energyIdx > 4) energyIdx = 0;
    uint16_t v = limits[energyIdx];
    f06_data[0] = v & 0xFF;
    f06_data[1] = (v >> 8) & 0xFF;
}

// Frame 0x07 pump/fan
static void f07_setPumpOrFan(uint8_t pumpOrFan, uint8_t* f07_data) {
    f07_data[0] = pumpOrFan | 0xE0;
    f07_data[1] = 0xFE;
}

TEST_CASE("Frame 0x20 room setpoint encoding", "[lin][frames]") {
    uint8_t f20_data[8] = {0};
    
    SECTION("20°C encodes correctly") {
        f20_setRoomSetpoint(20.0, f20_data);
        uint16_t raw = (uint16_t)((20.0 + 273.0) * 10.0); // 2930 = 0x0B72
        REQUIRE(f20_data[0] == 0x72);
        REQUIRE(f20_data[1] == (0xA0 | 0x0B));
    }
    
    SECTION("5°C (minimum) encodes correctly") {
        f20_setRoomSetpoint(5.0, f20_data);
        uint16_t raw = (uint16_t)((5.0 + 273.0) * 10.0); // 2780 = 0x0ADC
        REQUIRE(f20_data[0] == 0xDC);
        REQUIRE(f20_data[1] == (0xA0 | 0x0A));
    }
    
    SECTION("30°C (maximum) encodes correctly") {
        f20_setRoomSetpoint(30.0, f20_data);
        uint16_t raw = (uint16_t)((30.0 + 273.0) * 10.0); // 3030 = 0x0BD6
        REQUIRE(f20_data[0] == 0xD6);
        REQUIRE(f20_data[1] == (0xA0 | 0x0B));
    }
    
    SECTION("Below 5°C sets sentinel 0xAA 0xAA") {
        f20_setRoomSetpoint(4.9, f20_data);
        REQUIRE(f20_data[0] == 0xAA);
        REQUIRE(f20_data[1] == 0xAA);
    }
    
    SECTION("Above 30°C sets sentinel 0xAA 0xAA") {
        f20_setRoomSetpoint(30.1, f20_data);
        REQUIRE(f20_data[0] == 0xAA);
        REQUIRE(f20_data[1] == 0xAA);
    }
}

TEST_CASE("Frame 0x20 water setpoint encoding", "[lin][frames]") {
    uint8_t f20_data[8] = {0};
    
    SECTION("40°C encodes correctly") {
        f20_setWaterSetpoint(40.0, f20_data);
        uint16_t raw = (uint16_t)((40.0 + 273.0) * 10.0); // 3130 = 0x0C3A
        REQUIRE(f20_data[2] == ((raw >> 4) & 0xFF));
    }
    
    SECTION("60°C encodes correctly") {
        f20_setWaterSetpoint(60.0, f20_data);
        uint16_t raw = (uint16_t)((60.0 + 273.0) * 10.0); // 3330 = 0x0D02
        REQUIRE(f20_data[2] == ((raw >> 4) & 0xFF));
    }
    
    SECTION("Below 1°C sets sentinel 0xAA") {
        f20_setWaterSetpoint(0.5, f20_data);
        REQUIRE(f20_data[2] == 0xAA);
    }
}

TEST_CASE("Frame 0x20 fan and water mode encoding", "[lin][frames]") {
    uint8_t f20_data[8] = {0};
    
    SECTION("Eco heat (pumpOrFan=1) with water off") {
        f20_setFanAndWater(1, 0, f20_data);
        REQUIRE(f20_data[5] == 0xB0);
    }
    
    SECTION("High heat (pumpOrFan=2) with water on") {
        f20_setFanAndWater(2, 1, f20_data);
        REQUIRE(f20_data[5] == 0xD1);
    }
    
    SECTION("Fan level 5 (pumpOrFan=0x15) with water off") {
        f20_setFanAndWater(0x15, 0, f20_data);
        REQUIRE(f20_data[5] == 0x50);
    }
    
    SECTION("Fan level 10 (pumpOrFan=0x1A) with water on") {
        f20_setFanAndWater(0x1A, 1, f20_data);
        REQUIRE(f20_data[5] == 0xA1);
    }
}

TEST_CASE("Frame 0x05 energy selection", "[lin][frames]") {
    uint8_t f05_data[8] = {0};
    
    SECTION("Gas only (idx=0) → priority 1") {
        f05_setEnergySelection(0, f05_data);
        REQUIRE(f05_data[0] == 1);
    }
    
    SECTION("Mix 900 (idx=1) → priority 3") {
        f05_setEnergySelection(1, f05_data);
        REQUIRE(f05_data[0] == 3);
    }
    
    SECTION("Mix 1800 (idx=2) → priority 3") {
        f05_setEnergySelection(2, f05_data);
        REQUIRE(f05_data[0] == 3);
    }
    
    SECTION("Elec 900 (idx=3) → priority 2") {
        f05_setEnergySelection(3, f05_data);
        REQUIRE(f05_data[0] == 2);
    }
    
    SECTION("Elec 1800 (idx=4) → priority 2") {
        f05_setEnergySelection(4, f05_data);
        REQUIRE(f05_data[0] == 2);
    }
    
    SECTION("Invalid index defaults to 0") {
        f05_setEnergySelection(-1, f05_data);
        REQUIRE(f05_data[0] == 1);
        
        f05_setEnergySelection(5, f05_data);
        REQUIRE(f05_data[0] == 1);
    }
}

TEST_CASE("Frame 0x06 power limit", "[lin][frames]") {
    uint8_t f06_data[8] = {0};
    
    SECTION("Gas only (idx=0) → 0W") {
        f06_setPowerLimit(0, f06_data);
        REQUIRE(f06_data[0] == 0x00);
        REQUIRE(f06_data[1] == 0x00);
    }
    
    SECTION("Mix 900 (idx=1) → 900W") {
        f06_setPowerLimit(1, f06_data);
        REQUIRE(f06_data[0] == (900 & 0xFF));
        REQUIRE(f06_data[1] == ((900 >> 8) & 0xFF));
    }
    
    SECTION("Elec 1800 (idx=4) → 1800W") {
        f06_setPowerLimit(4, f06_data);
        REQUIRE(f06_data[0] == (1800 & 0xFF));
        REQUIRE(f06_data[1] == ((1800 >> 8) & 0xFF));
    }
}

TEST_CASE("Frame 0x07 pump/fan", "[lin][frames]") {
    uint8_t f07_data[8] = {0};
    
    SECTION("Fan level 5 (0x15)") {
        f07_setPumpOrFan(0x15, f07_data);
        REQUIRE(f07_data[0] == (0x15 | 0xE0));
        REQUIRE(f07_data[1] == 0xFE);
    }
    
    SECTION("Eco heat (1)") {
        f07_setPumpOrFan(1, f07_data);
        REQUIRE(f07_data[0] == (1 | 0xE0));
        REQUIRE(f07_data[1] == 0xFE);
    }
}
