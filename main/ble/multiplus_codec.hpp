#pragma once
#include <cstdint>
#include "multiplusble.hpp"   // MultiplusData + MULTI_POWER_NA (IDF-free)

// Pure VE.Bus / Multiplus Instant-Readout bitstream decode (no ESP-IDF / NimBLE
// deps) so it can be host-tested against the real code — see
// test/host/test_multiplus_{bitreader,parser}.cpp. The AES-CTR decryption and
// the NimBLE advert plumbing stay in multiplusble.cpp; this module takes the
// already-decrypted 16-byte plaintext.

// LSB-first packed reader over up to 16 bytes (128 bits). readU(n) consumes the
// next n bits as unsigned; readS(n) interprets them in two's complement.
struct BitReader {
    uint64_t lo;     // bits 0..63
    uint64_t hi;     // bits 64..127
    int      pos;

    BitReader(const uint8_t* p) : pos(0) {
        lo = 0; hi = 0;
        for (int i = 0; i < 8; i++) lo |= ((uint64_t)p[i])     << (i * 8);
        for (int i = 0; i < 8; i++) hi |= ((uint64_t)p[8 + i]) << (i * 8);
    }

    uint64_t readU(int bits) {
        uint64_t v;
        if (pos + bits <= 64) {
            v = (lo >> pos) & ((bits < 64) ? ((1ULL << bits) - 1) : ~0ULL);
        } else if (pos >= 64) {
            int p = pos - 64;
            v = (hi >> p) & ((bits < 64) ? ((1ULL << bits) - 1) : ~0ULL);
        } else {
            int loBits = 64 - pos;
            int hiBits = bits - loBits;
            uint64_t loPart = lo >> pos;
            uint64_t hiPart = hi & ((1ULL << hiBits) - 1);
            v = loPart | (hiPart << loBits);
        }
        pos += bits;
        return v;
    }

    int64_t readS(int bits) {
        uint64_t v = readU(bits);
        if (bits < 64) {
            uint64_t signbit = 1ULL << (bits - 1);
            if (v & signbit) {
                v |= ~((1ULL << bits) - 1);   // sign-extend
            }
        }
        return (int64_t)v;
    }
};

// Unpack a decrypted 16-byte VE.Bus plaintext record into `out`, applying the
// per-field "not available" sentinels. Sets out.valid = true; leaves out.lastMs
// untouched (the caller timestamps it).
void multiplusParseRecord(const uint8_t* pt, MultiplusData& out);
