#include "ultimatron_codec.hpp"

bool ultimatronParseResponse(const uint8_t* d, int len, UltimatronData& out) {
    if (len < 27 || d[0] != 0xdd || d[1] != 0x03 || d[2] != 0x00) return false;
    uint16_t rawV = ((uint16_t)d[4] << 8) | d[5];
    int16_t  rawA = (int16_t)(((uint16_t)d[6] << 8) | d[7]);
    out = {};
    out.battV = rawV / 100.0f;
    out.battA = rawA / 100.0f;
    out.soc   = d[23];
    out.valid = true;
    if (len >= 30) {
        int16_t rawT = (int16_t)(((uint16_t)d[27] << 8) | d[28]);
        out.tempC = (rawT - 2731) / 10.0f;
    }
    return true;
}
