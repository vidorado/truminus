#pragma once

#include <cstdint>
#include "control_state.hpp"

// Fan mode conversions
// 0=off, 1=eco, 2=high, 3..12 = level 1..10
int fanStrToInt(const char* v);
const char* fanIntToStr(int m);

// Boiler mode conversions
// 0=off, 1=eco, 2=high, 3=boost
int boilerStrToInt(const char* v);
const char* boilerIntToStr(int m);

struct TrumaLinSetpoints {
    uint8_t pumpOrFan;
    double roomSp;
    double waterSp;
};

/**
 * Deriva los setpoints LIN del estado de control actual.
 * 
 * Convierte el estado de la UI (heating/fan/boiler) a los valores
 * que el protocolo LIN de Truma espera:
 * - pumpOrFan: modo de ventilador/bomba (0x10-0x1A para fan-only, 1-2 para heating)
 * - roomSp: temperatura objetivo de habitación en °C (0 si heating off)
 * - waterSp: temperatura objetivo de agua en °C (40/60 según boilerMode)
 */
TrumaLinSetpoints derive_mode(const P4ControlState& cs);
