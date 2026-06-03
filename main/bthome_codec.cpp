#include "bthome_codec.hpp"

bool parseBthomePayload(const uint8_t* p, int len, uint8_t* pct, uint8_t* seq) {
    // BTHome tag table (only the subset we may encounter): each tag's value
    // length is implied by the tag itself, used to skip unknown ones safely.
    bool gotMoisture = false;
    int i = 0;
    while (i < len) {
        uint8_t tag = p[i++];
        int size;
        switch (tag) {
            case 0x00: size = 1; break;     // packet ID
            case 0x01: size = 1; break;     // battery %
            case 0x02: size = 2; break;     // temperature sint16
            case 0x03: size = 2; break;     // humidity uint16
            case 0x12: size = 2; break;     // CO2 uint16
            case 0x14: size = 2; break;     // moisture uint16
            case 0x2E: size = 1; break;     // humidity uint8
            case 0x2F: size = 1; break;     // moisture uint8  ← we want this
            case 0x4A: size = 2; break;     // voltage uint16
            case 0x51: size = 1; break;     // count uint8
            default:
                // Unknown tag of unknown length — bail rather than mis-parse.
                return gotMoisture;
        }
        if (i + size > len) return gotMoisture;
        if (tag == 0x2F) {
            *pct = p[i];
            gotMoisture = true;
        } else if (tag == 0x00) {
            *seq = p[i];
        }
        i += size;
    }
    return gotMoisture;
}
