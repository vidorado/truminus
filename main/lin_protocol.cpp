#include "lin_protocol.hpp"

uint8_t linProtectedId(uint8_t frameId) {
    uint8_t p0 = ((frameId >> 0) & 1) ^ ((frameId >> 1) & 1) ^
                 ((frameId >> 2) & 1) ^ ((frameId >> 4) & 1);
    uint8_t p1 = ~(((frameId >> 1) & 1) ^ ((frameId >> 3) & 1) ^
                   ((frameId >> 4) & 1) ^ ((frameId >> 5) & 1));
    return (uint8_t)((p1 << 7) | (p0 << 6) | (frameId & 0x3F));
}

uint8_t linChecksum(uint8_t protectedId, const uint8_t* data, uint8_t dataLen) {
    uint16_t sum = protectedId;
    if ((sum & 0x3F) >= 0x3C) sum = 0x00;   // classic checksum for diag frames
    while (dataLen-- > 0) sum += data[dataLen];
    while (sum >> 8) sum = (sum & 0xFF) + (sum >> 8);
    return (uint8_t)(~sum);
}
