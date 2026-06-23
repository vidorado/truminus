#include "faultlog_codec.hpp"

// Mirror of the esp_reset_reason_t values used here (stable ESP-IDF ABI). Kept
// local so this module stays free of esp_system.h and remains host-testable.
enum {
    RST_UNKNOWN   = 0,
    RST_POWERON   = 1,
    RST_EXT       = 2,
    RST_SW        = 3,
    RST_PANIC     = 4,
    RST_INT_WDT   = 5,
    RST_TASK_WDT  = 6,
    RST_WDT       = 7,
    RST_DEEPSLEEP = 8,
    RST_BROWNOUT  = 9,
    RST_SDIO      = 10,
    RST_USB       = 11,
    RST_JTAG      = 12,
};

bool faultIsCrash(int r) {
    return r == RST_PANIC   || r == RST_TASK_WDT ||
           r == RST_INT_WDT || r == RST_WDT;
}

const char* faultReasonName(int r) {
    switch (r) {
        case RST_POWERON:   return "power-on";
        case RST_EXT:       return "external";
        case RST_SW:        return "sw-restart";
        case RST_PANIC:     return "panic/abort";
        case RST_INT_WDT:   return "int-wdt";
        case RST_TASK_WDT:  return "task-wdt";
        case RST_WDT:       return "wdt";
        case RST_DEEPSLEEP: return "deep-sleep";
        case RST_BROWNOUT:  return "brownout";
        case RST_SDIO:      return "sdio";
        case RST_USB:       return "usb";
        case RST_JTAG:      return "jtag";
        case FAULT_RSN_OTA_ROLLBACK: return "OTA rollback";
        default:            return "unknown";
    }
}
