#include "catch_amalgamated.hpp"
#include "test_helpers.hpp"

TEST_CASE("BTHome payload parsing", "[bthome]") {
    SECTION("Moisture tag 0x2F with value 75%") {
        uint8_t payload[] = {0x2F, 75};
        uint8_t pct = 0, seq = 0xFF;
        bool result = parseBthomePayload(payload, sizeof(payload), &pct, &seq);
        REQUIRE(result == true);
        REQUIRE(pct == 75);
    }

    SECTION("Packet ID tag 0x00 with value 42") {
        uint8_t payload[] = {0x00, 42};
        uint8_t pct = 0xFF, seq = 0xFF;
        bool result = parseBthomePayload(payload, sizeof(payload), &pct, &seq);
        REQUIRE(result == false);
        REQUIRE(seq == 42);
    }

    SECTION("Both packet ID and moisture") {
        uint8_t payload[] = {0x00, 10, 0x2F, 85};
        uint8_t pct = 0, seq = 0;
        bool result = parseBthomePayload(payload, sizeof(payload), &pct, &seq);
        REQUIRE(result == true);
        REQUIRE(pct == 85);
        REQUIRE(seq == 10);
    }

    SECTION("Temperature tag 0x02 (2 bytes)") {
        uint8_t payload[] = {0x02, 0x10, 0x01, 0x2F, 50};
        uint8_t pct = 0, seq = 0;
        bool result = parseBthomePayload(payload, sizeof(payload), &pct, &seq);
        REQUIRE(result == true);
        REQUIRE(pct == 50);
    }

    SECTION("Unknown tag stops parsing") {
        uint8_t payload[] = {0xFF, 0x2F, 60};
        uint8_t pct = 0, seq = 0;
        bool result = parseBthomePayload(payload, sizeof(payload), &pct, &seq);
        REQUIRE(result == false);
    }

    SECTION("Truncated payload returns false") {
        uint8_t payload[] = {0x02, 0x10};
        uint8_t pct = 0, seq = 0;
        bool result = parseBthomePayload(payload, sizeof(payload), &pct, &seq);
        REQUIRE(result == false);
    }

    SECTION("Empty payload") {
        uint8_t payload[] = {};
        uint8_t pct = 0, seq = 0;
        bool result = parseBthomePayload(payload, 0, &pct, &seq);
        REQUIRE(result == false);
    }

    SECTION("Multiple moisture tags - last one wins") {
        uint8_t payload[] = {0x2F, 30, 0x2F, 90};
        uint8_t pct = 0, seq = 0;
        bool result = parseBthomePayload(payload, sizeof(payload), &pct, &seq);
        REQUIRE(result == true);
        REQUIRE(pct == 90);
    }
}
