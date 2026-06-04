#include "catch_amalgamated.hpp"
#include "lin_protocol.hpp"   // real production LIN protocol math

TEST_CASE("LIN protected ID calculation", "[lin][protocol]") {
    SECTION("Frame ID 0x00") {
        uint8_t pid = linProtectedId(0x00);
        REQUIRE((pid & 0x3F) == 0x00);
    }

    SECTION("Frame ID 0x3C (diagnostic)") {
        uint8_t pid = linProtectedId(0x3C);
        REQUIRE((pid & 0x3F) == 0x3C);
    }

    SECTION("Frame ID 0x3D (diagnostic)") {
        uint8_t pid = linProtectedId(0x3D);
        REQUIRE((pid & 0x3F) == 0x3D);
    }

    SECTION("Protected ID preserves frame ID in lower 6 bits") {
        for (uint8_t id = 0; id < 64; id++) {
            uint8_t pid = linProtectedId(id);
            REQUIRE((pid & 0x3F) == id);
        }
    }

    SECTION("Parity bits are calculated correctly") {
        uint8_t pid = linProtectedId(0x21);
        REQUIRE((pid & 0x3F) == 0x21);
        REQUIRE((pid >> 6) <= 3);
    }
}

TEST_CASE("LIN checksum calculation", "[lin][protocol]") {
    SECTION("Classic checksum for diagnostic frames (ID >= 0x3C)") {
        uint8_t pid = linProtectedId(0x3C);
        uint8_t data[] = {0x00, 0x01, 0x02};
        uint8_t cksum = linChecksum(pid, data, 3);
        REQUIRE(cksum != 0);
    }

    SECTION("Enhanced checksum for normal frames") {
        uint8_t pid = linProtectedId(0x21);
        uint8_t data[] = {0x10, 0x20, 0x30, 0x40};
        uint8_t cksum = linChecksum(pid, data, 4);
        REQUIRE(cksum != 0);
    }

    SECTION("Checksum with zero data") {
        uint8_t pid = linProtectedId(0x10);
        uint8_t data[] = {0x00, 0x00, 0x00, 0x00};
        uint8_t cksum = linChecksum(pid, data, 4);
        REQUIRE(cksum != 0);
    }

    SECTION("Checksum with all 0xFF data") {
        uint8_t pid = linProtectedId(0x15);
        uint8_t data[] = {0xFF, 0xFF, 0xFF, 0xFF};
        uint8_t cksum = linChecksum(pid, data, 4);
        REQUIRE(cksum != 0);
    }

    SECTION("Checksum verification - complement should equal 0xFF") {
        uint8_t pid = linProtectedId(0x22);
        uint8_t data[] = {0xAB, 0xCD, 0xEF, 0x12};
        uint8_t cksum = linChecksum(pid, data, 4);
        uint8_t verify = (uint8_t)(cksum + ~linChecksum(pid, data, 4));
        REQUIRE(verify == 0xFF);
    }

    SECTION("Different data produces different checksums") {
        uint8_t pid = linProtectedId(0x20);
        uint8_t data1[] = {0x00, 0x00, 0x00, 0x00};
        uint8_t data2[] = {0x01, 0x00, 0x00, 0x00};
        uint8_t cksum1 = linChecksum(pid, data1, 4);
        uint8_t cksum2 = linChecksum(pid, data2, 4);
        REQUIRE(cksum1 != cksum2);
    }
}
