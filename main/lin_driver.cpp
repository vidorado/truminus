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
    startTransmission(protectedId);
    uart_wait_tx_done(m_uart, pdMS_TO_TICKS(20));

    if (expectedLen > 0) {
        // Wait up to 400 ms for expectedLen + echo bytes to arrive.
        uint32_t t0 = millis_now();
        while ((millis_now() - t0) < 400) {
            size_t avail = 0;
            uart_get_buffered_data_len(m_uart, &avail);
            if (avail >= (size_t)(expectedLen + 4)) break;
            vTaskDelay(1);
        }
    } else {
        delay_ms(100);
    }

    // Parse: skip break(0x00) + sync(0x55) + PID echo, then collect data.
    int bytesReceived = -4;
    size_t avail = 0;
    uart_get_buffered_data_len(m_uart, &avail);

    while (avail > 0 && bytesReceived < (8 + 1)) {
        uint8_t b = 0;
        if (uart_read_bytes(m_uart, &b, 1, 0) != 1) break;
        rxBytesTotal++;
        if (bytesReceived < 0) {
            if (b == 0x00) { bytesReceived = -3; }
            if (b == 0x55) { bytesReceived = -2; }
            if (b == protectedId) { bytesReceived = -1; }
        } else {
            LinMessage[bytesReceived] = b;
        }
        bytesReceived++;
        uart_get_buffered_data_len(m_uart, &avail);
    }

    if (bytesReceived <= 0) {
        // Drain anything that did arrive into a scratch buffer so we can
        // dump the raw bytes — helps diagnose "slave silent" vs "slave
        // replied but our parser tossed it".  Rate-limited to one dump
        // per ~3 s per FID to keep the monitor readable.
        uint8_t leftover[24]; size_t n = 0;
        while (n < sizeof(leftover) &&
               uart_read_bytes(m_uart, &leftover[n], 1, 0) == 1) { n++; rxBytesTotal++; }
        if (verboseMode > 0) {
            static uint32_t lastLogMs[256] = {0};
            uint32_t nowMs = millis_now();
            if ((uint32_t)(nowMs - lastLogMs[frameId]) > 3000) {
                lastLogMs[frameId] = nowMs;
                if (n == 0) {
                    ESP_LOGI(TAG, "FID %02X (PID %02X) — no bytes back", frameId, protectedId);
                } else {
                    char hex[3*24+1]; size_t off = 0;
                    for (size_t i = 0; i < n; i++) off += snprintf(hex+off, sizeof(hex)-off, "%02X ", leftover[i]);
                    ESP_LOGI(TAG, "FID %02X (PID %02X) — got %u bytes: %s",
                             frameId, protectedId, (unsigned)n, hex);
                }
            }
        }
        return false;
    }

    uint8_t checksum = LinMessage[bytesReceived - 1];
    bytesReceived--;

    // Drain leftover bytes.
    uint8_t discard;
    while (uart_read_bytes(m_uart, &discard, 1, 0) == 1) {
        rxBytesTotal++;
        if (verboseMode > 0) ESP_LOGD(TAG, "extra byte discarded");
    }

    bool ok = (0xFF == (uint8_t)(checksum + ~getChecksum(protectedId, bytesReceived)));

    if (verboseMode > 0) {
        ESP_LOGD(TAG, " --->> FID %02Xh [%02X] rcv=%d%s",
                 frameId, protectedId, bytesReceived, ok ? "" : " CHKFAIL");
    }
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
