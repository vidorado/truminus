#include "lin_driver.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "driver/gpio.h"

static const char* TAG = "lin";

// ── Helpers ───────────────────────────────────────────────────────────────

static inline uint32_t millis_now() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static inline void delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// ── Constructor ───────────────────────────────────────────────────────────

LinDriver::LinDriver(uart_port_t uart_nr, int tx_pin, int rx_pin, uint32_t baud)
    : m_uart(uart_nr), m_txPin(tx_pin), m_rxPin(rx_pin), m_baud(baud) {}

void LinDriver::ensureStarted() {
    if (m_started) return;

    uart_config_t cfg = {};
    cfg.baud_rate  = (int)m_baud;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    // ESP32-P4: UART_SCLK_DEFAULT picks PLL_F80M which produces ~0.5-1%
    // baudrate skew at 9600 — enough to make the Truma slave drop every
    // request.  XTAL (40 MHz) divides cleanly to 9600 with <0.01% error.
    cfg.source_clk = UART_SCLK_XTAL;

    // IDF 6.0 canonical order: install driver before param_config / set_pin.
    // RX buffer 256 bytes, no TX buffer, no event queue.
    ESP_ERROR_CHECK(uart_driver_install(m_uart, 256, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(m_uart, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(m_uart, m_txPin, m_rxPin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // Force an internal pull-up on RX so the line idles HIGH if the
    // transceiver's RXD output is ever tri-stated (e.g. boot, sleep
    // transitions).  uart_set_pin doesn't set pulls on its own.
    gpio_set_pull_mode((gpio_num_t)m_rxPin, GPIO_PULLUP_ONLY);

    // TX drives the transceiver's TXD input.  The TJA1020 TXD pin has only a
    // weak INTERNAL PULL-DOWN (125–800 kΩ, datasheet) and no pull-up, so the
    // recessive (HIGH) level depends entirely on the host driving it up.  On
    // the P4 the recessive edge rose slowly/rounded and the recessive bits of
    // our own 0x55 sagged below the TXD threshold (VIH = 2 V) before the UART's
    // mid-bit sample point → we read 0x00 and every frame failed its checksum.
    // The fix is a HARDWARE PULL-UP on TXD (4.7 kΩ to 5 V works; ~2.2–4.7 kΩ to
    // 3V3 is cleaner — only >2 V is needed and it avoids over-driving the P4
    // pad).  See README "Hardware: P4 ↔ Truma LIN wiring".  We also crank the
    // pad drive strength here (harmless; helps the rising edge a little).
    // Forcing the pad push-pull via gpio_set_direction() is NOT a substitute —
    // it breaks the UART matrix routing and RX goes silent.
    gpio_set_drive_capability((gpio_num_t)m_txPin, GPIO_DRIVE_CAP_3);
    ESP_LOGI(TAG, "UART%d configured: TX=GPIO%d RX=GPIO%d @ %lu baud",
             (int)m_uart, m_txPin, m_rxPin, (unsigned long)m_baud);
    m_started = true;
}

// ── LIN bus commands ──────────────────────────────────────────────────────

void LinDriver::writeCmdWakeup() {
    ensureStarted();
    uart_wait_tx_done(m_uart, pdMS_TO_TICKS(20));
    uart_set_baudrate(m_uart, m_baud >> 1);
    uint8_t zero = 0x00;
    uart_write_bytes(m_uart, &zero, 1);
    uart_wait_tx_done(m_uart, pdMS_TO_TICKS(20));
    uart_set_baudrate(m_uart, m_baud);
    delay_ms(150);
}

void LinDriver::writeCmdSleep() {
    LinMessage[0] = 0;
    for (int i = 1; i <= 7; ++i) LinMessage[i] = 0xFF;
    writeFrame(0x3C, 8);
}

// ── Loopback self-test ─────────────────────────────────────────────────────
//
// Transmits known bytes and dumps the half-duplex echo so we can tell apart:
//   * Test A (0x55 ×4, NO break)         — if echo == "55 55 55 55", the RX
//     baud/clock and the transceiver loopback are fine.  If smeared, the
//     UART baud or source clock is wrong.
//   * Test B (break + 0x55 + 0x61 + data) — same header readFrame() sends.  If
//     A is clean but B is smeared from the 0x55 on, the half-baud break leaves
//     the UART desynced when we switch back to normal baud.
static void dumpRx(const char* tag, const uint8_t* b, int n) {
    char hex[3 * 32 + 1]; size_t off = 0;
    for (int i = 0; i < n && i < 32; i++)
        off += snprintf(hex + off, sizeof(hex) - off, "%02X ", b[i]);
    ESP_LOGW(TAG, "selftest %s: echo[%d]=%s", tag, n, hex);
}

void LinDriver::selfTestLoopback() {
    ensureStarted();
    uint8_t rx[48];
    uint8_t pat[4] = {0x55, 0x55, 0x55, 0x55};

    // Baud sweep: transmit 0x55x4 at each rate and dump the half-duplex echo.
    // 0x55 is the max-transition pattern, so the rate at which it loops back
    // as a clean "55 55 55 55" reveals the true effective link baud:
    //   * exactly one rate clean      → UART clock/baud was misconfigured
    //   * none clean (lows less smeared as rate drops) → bus too slow for 9600
    //     (LIN pull-up / transceiver Vsup / slew-rate — a hardware issue)
    const uint32_t bauds[] = {2400, 4800, 9600, 19200, 38400};
    for (uint32_t b : bauds) {
        uart_set_baudrate(m_uart, b);
        uart_flush_input(m_uart);
        uart_write_bytes(m_uart, pat, 4);
        uart_wait_tx_done(m_uart, pdMS_TO_TICKS(40));
        delay_ms(40);
        int n = 0;
        while (n < (int)sizeof(rx) && uart_read_bytes(m_uart, &rx[n], 1, 0) == 1) n++;
        char tag[24]; snprintf(tag, sizeof(tag), "0x55x4 @ %lu", (unsigned long)b);
        dumpRx(tag, rx, n);
    }
    uart_set_baudrate(m_uart, m_baud);   // restore configured baud
}

// ── Internal frame helpers ────────────────────────────────────────────────

void LinDriver::sendBreak() {
    // Half-baud 0x00 trick — write a zero at baud/2, equivalent to an 18-bit
    // dominant pulse at normal baud.  LIN spec minimum break is 13 bits.
    // The native uart_write_bytes_with_break() API appends break AFTER
    // data, so it can't generate a leading break without a dummy prefix
    // byte; we tried that variant and saw no behaviour change.  This is
    // the same break the Arduino driver on the C5 board produces.
    uart_wait_tx_done(m_uart, pdMS_TO_TICKS(10));
    uart_set_baudrate(m_uart, m_baud >> 1);
    uint8_t zero = 0x00;
    uart_write_bytes(m_uart, &zero, 1);
    uart_wait_tx_done(m_uart, pdMS_TO_TICKS(10));
    uart_set_baudrate(m_uart, m_baud);
}

void LinDriver::startTransmission(uint8_t protectedId) {
    ensureStarted();
    sendBreak();
    uint8_t header[2] = {0x55, protectedId};
    uart_write_bytes(m_uart, header, 2);
}

// ── Public frame API ──────────────────────────────────────────────────────

bool LinDriver::readFrame(uint8_t frameId, uint8_t expectedLen) {
    uint8_t protectedId = getProtectedId(frameId);

    // Discard any stale RX (previous frame's TX echo / partials) so the only
    // thing in the buffer is this transaction's echo + the slave response.
    uart_flush_input(m_uart);

    startTransmission(protectedId);
    uart_wait_tx_done(m_uart, pdMS_TO_TICKS(20));

    if (expectedLen == 0) { delay_ms(100); return false; }

    // Collect raw bytes for up to 400 ms.  We can NOT use a fixed byte count
    // to decide the frame is complete: the half-baud break (0x00 @ baud/2)
    // loops back through the half-duplex transceiver and the IDF UART decodes
    // it as 1 OR 2 bytes (framing error, non-deterministic), so the echo
    // prefix length varies.  Instead we scan for the (0x55, PID) header — both
    // are sent at normal baud and always arrive clean, regardless of break
    // junk before them — and read exactly expectedLen data + 1 checksum after
    // it, finishing early on a bus-idle gap.
    uint8_t raw[48];
    int rn = 0;
    int dataStart = -1;            // index just past the (0x55, PID) header
    uint32_t t0 = millis_now();
    while ((millis_now() - t0) < 400) {
        uint8_t b;
        if (uart_read_bytes(m_uart, &b, 1, pdMS_TO_TICKS(5)) == 1) {
            rxBytesTotal++;
            if (rn < (int)sizeof(raw)) raw[rn++] = b;
            if (dataStart < 0 && rn >= 2 &&
                raw[rn - 2] == 0x55 && raw[rn - 1] == protectedId) {
                dataStart = rn;
            }
            // Stop as soon as we have the whole frame (data + checksum).
            if (dataStart >= 0 && (rn - dataStart) >= (int)expectedLen + 1) break;
        } else if (dataStart >= 0 && rn - dataStart >= 1) {
            break;                 // idle gap after a partial response → done
        }
    }

    int got = (dataStart >= 0) ? (rn - dataStart) : -1;

    auto diagDump = [&](const char* note) {
        if (verboseMode <= 0) return;
        static uint32_t lastLogMs[256] = {0};
        uint32_t nowMs = millis_now();
        if ((uint32_t)(nowMs - lastLogMs[frameId]) <= 1000) return;
        lastLogMs[frameId] = nowMs;
        char hex[3 * 48 + 1]; size_t off = 0; hex[0] = '\0';
        for (int i = 0; i < rn; i++)
            off += snprintf(hex + off, sizeof(hex) - off, "%02X ", raw[i]);
        // DEBUG level: a silent slave (0x3D in idle) is normal, so don't spam
        // INFO.  Raise the "lin" tag to DEBUG to see raw bytes + checksum.
        ESP_LOGD(TAG, "FID %02X (PID %02X) %s raw[%d]=%s",
                 frameId, protectedId, note, rn, hex);
    };

    if (got < (int)expectedLen + 1) {
        // No header at all → slave silent; header but short → truncated reply.
        diagDump(dataStart < 0 ? "no response" : "truncated");
        return false;
    }

    memcpy(LinMessage, raw + dataStart, expectedLen);
    uint8_t checksum = raw[dataStart + expectedLen];
    bool ok = (0xFF == (uint8_t)(checksum + ~getChecksum(protectedId, expectedLen)));

    if (verboseMode > 0 && !ok) diagDump("CHKFAIL");
    return ok;
}

void LinDriver::writeFrame(uint8_t frameId, uint8_t dataLen) {
    uint8_t protectedId = getProtectedId(frameId);
    uint8_t cksum = getChecksum(protectedId, dataLen);

    startTransmission(protectedId);
    uart_write_bytes(m_uart, LinMessage, dataLen);
    uart_write_bytes(m_uart, &cksum, 1);
    delay_ms(20);

    // Consume echo (TX is looped back to RX on half-duplex LIN transceivers).
    uint8_t discard;
    while (uart_read_bytes(m_uart, &discard, 1, 0) == 1) { rxBytesTotal++; }
}

void LinDriver::writeFrameClassic(uint8_t frameId, uint8_t dataLen) {
    uint8_t protectedId = getProtectedId(frameId);
    uint8_t cksum = getChecksum(0x00, dataLen);
    startTransmission(protectedId);
    uart_write_bytes(m_uart, LinMessage, dataLen);
    uart_write_bytes(m_uart, &cksum, 1);
    uart_wait_tx_done(m_uart, pdMS_TO_TICKS(20));
}

void LinDriver::writeFrameClassicNoChecksum(uint8_t frameId, uint8_t dataLen) {
    uint8_t protectedId = getProtectedId(frameId);
    startTransmission(protectedId);
    uart_write_bytes(m_uart, LinMessage, dataLen);
    uart_wait_tx_done(m_uart, pdMS_TO_TICKS(20));
}

// ── Protocol helpers ──────────────────────────────────────────────────────

uint8_t LinDriver::getProtectedId(uint8_t frameId) {
    uint8_t p0 = ((frameId >> 0) & 1) ^ ((frameId >> 1) & 1) ^
                 ((frameId >> 2) & 1) ^ ((frameId >> 4) & 1);
    uint8_t p1 = ~(((frameId >> 1) & 1) ^ ((frameId >> 3) & 1) ^
                   ((frameId >> 4) & 1) ^ ((frameId >> 5) & 1));
    return (uint8_t)((p1 << 7) | (p0 << 6) | (frameId & 0x3F));
}

uint8_t LinDriver::getChecksum(uint8_t protectedId, uint8_t dataLen) {
    uint16_t sum = protectedId;
    if ((sum & 0x3F) >= 0x3C) sum = 0x00;   // classic checksum for diag frames
    while (dataLen-- > 0) sum += LinMessage[dataLen];
    while (sum >> 8) sum = (sum & 0xFF) + (sum >> 8);
    return (uint8_t)(~sum);
}
