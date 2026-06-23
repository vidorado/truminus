#include "lin_frames.hpp"
#include <cmath>

void linF20Room(double celsius, uint8_t* f20) {
    if (celsius < 5.0 || celsius > 30.0) {
        f20[0] = 0xAA;
        f20[1] = 0xAA;
        return;
    }
    uint16_t raw = (uint16_t)lround((celsius + 273.0) * 10.0);
    f20[0] = raw & 0xFF;
    f20[1] = 0xA0 | ((raw >> 8) & 0x0F);
}

void linF20Water(double celsius, uint8_t* f20) {
    if (celsius < 1.0) { f20[2] = 0xAA; return; }
    uint16_t raw = (uint16_t)lround((celsius + 273.0) * 10.0);
    f20[2] = (uint8_t)(raw >> 4);
}

void linF20FanWater(uint8_t pumpOrFan, uint8_t waterMode, uint8_t* f20) {
    uint8_t fanNibble;
    if (pumpOrFan == 1) {
        fanNibble = 0xB;
    } else if (pumpOrFan == 2) {
        fanNibble = 0xD;
    } else {
        uint8_t level = (pumpOrFan >= 0x10) ? (pumpOrFan & 0x0F) : pumpOrFan;
        fanNibble = level;
    }
    f20[5] = (fanNibble << 4) | (waterMode & 0x0F);
}

void linF05Energy(int energyIdx, uint8_t* f05) {
    // P4 indices map 1:1 to legacy TEnergySelection (Gas, Mix900, Mix1800,
    // Elec900, Elec1800). Priority codes: 1=EpFuel, 2=EpBothPrioElectro,
    // 3=EpBothPrioFuel.
    static const uint8_t priorities[5] = { 1, 3, 3, 2, 2 };
    if (energyIdx < 0 || energyIdx > 4) energyIdx = 0;
    f05[0] = priorities[energyIdx];
}

void linF06PowerLimit(int energyIdx, uint8_t* f06) {
    static const uint16_t limits[5] = { 0, 900, 1800, 900, 1800 };
    if (energyIdx < 0 || energyIdx > 4) energyIdx = 0;
    uint16_t v = limits[energyIdx];
    f06[0] = v & 0xFF;          // little-endian, matching the P4 (RV32 LE)
    f06[1] = (v >> 8) & 0xFF;
}

void linF07PumpOrFan(uint8_t pumpOrFan, uint8_t* f07) {
    f07[0] = pumpOrFan | 0xE0;
    f07[1] = 0xFE;
}
