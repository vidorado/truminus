#include "victron_codec.hpp"

bool victronParseRecord(const uint8_t* pt, VictronData& out) {
    int16_t  rawV   = (int16_t)((uint16_t)pt[2] | ((uint16_t)pt[3] << 8));
    int16_t  rawA   = (int16_t)((uint16_t)pt[4] | ((uint16_t)pt[5] << 8));
    uint16_t rawKwh = (uint16_t)pt[6] | ((uint16_t)pt[7] << 8);
    uint16_t rawPv  = (uint16_t)pt[8] | ((uint16_t)pt[9] << 8);

    if (rawV == 0x7FFF) return false;   // voltage "not available" sentinel

    out.state    = pt[0];
    out.errCode  = pt[1];
    out.battV    = rawV * 0.01f;
    out.battA    = (rawA == 0x7FFF) ? 0.0f : rawA * 0.1f;
    out.kWhToday = rawKwh * 0.01f;
    out.pvW      = (float)rawPv;
    out.valid    = true;
    return true;
}
