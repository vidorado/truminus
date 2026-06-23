#include "multiplus_codec.hpp"
#include <cmath>

void multiplusParseRecord(const uint8_t* pt, MultiplusData& out) {
    BitReader r(pt);
    uint8_t  ds   = (uint8_t)r.readU(8);
    uint8_t  er   = (uint8_t)r.readU(8);
    int16_t  rawA = (int16_t)r.readS(16);                // 0.1 A
    uint16_t rawV = (uint16_t)r.readU(14);               // 0.01 V
    uint8_t  ai   = (uint8_t)r.readU(2);
    int32_t  rawInW  = (int32_t)r.readS(19);
    int32_t  rawOutW = (int32_t)r.readS(19);
    uint8_t  al   = (uint8_t)r.readU(2);
    uint8_t  rawT = (uint8_t)r.readU(7);                 // °C - 40
    uint8_t  socR = (uint8_t)r.readU(7);

    out.deviceState = ds;
    out.error       = er;
    out.acInState   = ai;
    out.alarm       = al;
    // 0x3FFFF is the 19-bit "not available" sentinel (inverter off / not
    // reporting); keep it distinct from a real 0 W reading.
    out.acInW       = (rawInW  == 0x3FFFF) ? MULTI_POWER_NA : rawInW;
    out.acOutW      = (rawOutW == 0x3FFFF) ? MULTI_POWER_NA : rawOutW;
    out.battA       = (rawA == 0x7FFF)  ? 0.0f           : rawA * 0.1f;
    out.battV       = (rawV == 0x3FFF)  ? NAN            : rawV * 0.01f;
    out.battTempC   = (rawT == 0x7F)    ? (int8_t)-128   : (int8_t)((int)rawT - 40);
    out.soc         = (socR == 0x7F)    ? 0xFF           : socR;
    out.valid       = true;
}
