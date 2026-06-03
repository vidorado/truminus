#include "catch_amalgamated.hpp"
#include "test_helpers.hpp"

enum esp_reset_reason_t {
    ESP_RST_UNKNOWN = 0,
    ESP_RST_POWERON = 1,
    ESP_RST_EXT = 2,
    ESP_RST_SW = 3,
    ESP_RST_PANIC = 4,
    ESP_RST_INT_WDT = 5,
    ESP_RST_TASK_WDT = 6,
    ESP_RST_WDT = 7,
    ESP_RST_DEEPSLEEP = 8,
    ESP_RST_BROWNOUT = 9,
    ESP_RST_SDIO = 10,
    ESP_RST_USB = 11,
    ESP_RST_JTAG = 12,
};

static bool is_fault(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_INT_WDT || r == ESP_RST_WDT;
}

static const char* faultReasonName(int r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "sw-restart";
        case ESP_RST_PANIC:     return "panic/abort";
        case ESP_RST_INT_WDT:   return "int-wdt";
        case ESP_RST_TASK_WDT:  return "task-wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deep-sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        case ESP_RST_USB:       return "usb";
        case ESP_RST_JTAG:      return "jtag";
        default:                return "unknown";
    }
}

TEST_CASE("Fault detection", "[faultlog]") {
    SECTION("Panic is a fault") {
        REQUIRE(is_fault(ESP_RST_PANIC));
    }

    SECTION("Task WDT is a fault") {
        REQUIRE(is_fault(ESP_RST_TASK_WDT));
    }

    SECTION("Interrupt WDT is a fault") {
        REQUIRE(is_fault(ESP_RST_INT_WDT));
    }

    SECTION("Generic WDT is a fault") {
        REQUIRE(is_fault(ESP_RST_WDT));
    }

    SECTION("Power-on is not a fault") {
        REQUIRE_FALSE(is_fault(ESP_RST_POWERON));
    }

    SECTION("Software restart is not a fault") {
        REQUIRE_FALSE(is_fault(ESP_RST_SW));
    }

    SECTION("Deep sleep is not a fault") {
        REQUIRE_FALSE(is_fault(ESP_RST_DEEPSLEEP));
    }

    SECTION("Brownout is not a fault") {
        REQUIRE_FALSE(is_fault(ESP_RST_BROWNOUT));
    }

    SECTION("Unknown is not a fault") {
        REQUIRE_FALSE(is_fault(ESP_RST_UNKNOWN));
    }
}

TEST_CASE("Fault reason names", "[faultlog]") {
    SECTION("Panic returns correct name") {
        REQUIRE(std::string(faultReasonName(ESP_RST_PANIC)) == "panic/abort");
    }

    SECTION("Task WDT returns correct name") {
        REQUIRE(std::string(faultReasonName(ESP_RST_TASK_WDT)) == "task-wdt");
    }

    SECTION("Power-on returns correct name") {
        REQUIRE(std::string(faultReasonName(ESP_RST_POWERON)) == "power-on");
    }

    SECTION("Software restart returns correct name") {
        REQUIRE(std::string(faultReasonName(ESP_RST_SW)) == "sw-restart");
    }

    SECTION("Unknown returns unknown") {
        REQUIRE(std::string(faultReasonName(ESP_RST_UNKNOWN)) == "unknown");
    }

    SECTION("Invalid reason returns unknown") {
        REQUIRE(std::string(faultReasonName(999)) == "unknown");
    }

    SECTION("All defined reasons have non-empty names") {
        for (int r = ESP_RST_POWERON; r <= ESP_RST_JTAG; r++) {
            const char* name = faultReasonName(r);
            REQUIRE(name != nullptr);
            REQUIRE(std::string(name).size() > 0);
        }
    }
}
