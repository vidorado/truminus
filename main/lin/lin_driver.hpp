#pragma once
// LIN bus driver — ESP-IDF UART implementation (driver/uart.h).
// Implements LIN break, sync, PID and enhanced checksum over half-duplex UART.

#include <cstdint>
#include "driver/uart.h"

class LinDriver {
public:
    // uart_nr: UART port (UART_NUM_1 recommended; UART_NUM_0 is the debug console)
    // tx_pin / rx_pin: GPIO numbers for LIN transceiver
    // baud: LIN baud rate (19200 for Truma Combi D)
    explicit LinDriver(uart_port_t uart_nr, int tx_pin, int rx_pin, uint32_t baud = 19200);

    // 8 data bytes + checksum + overflow guard
    uint8_t LinMessage[8 + 1 + 4] = {0};

    int verboseMode = -1;

    // Diagnostic: total bytes consumed from the UART RX FIFO since boot
    // (echoes of our own TX + any slave reply).  Non-zero = the line is
    // physically alive (RX pin sees activity, transceiver powered);
    // zero = RX pin / transceiver / wiring is dead.
    uint32_t rxBytesTotal = 0;

    void writeCmdWakeup();
    void writeCmdSleep();

    // One-shot diagnostic: transmits known patterns and dumps the half-duplex
    // loopback echo, to separate "wrong RX baud/clock" from "break desync".
    void selfTestLoopback();

    // Request frame from slave (master → slave request, slave → master response).
    // Returns true if a valid frame with correct checksum was received.
    bool readFrame(uint8_t frameId, uint8_t expectedLen = 0);

    // Send frame to slave (master → slave command).
    void writeFrame(uint8_t frameId, uint8_t dataLen);
    void writeFrameClassic(uint8_t frameId, uint8_t dataLen);
    void writeFrameClassicNoChecksum(uint8_t frameId, uint8_t dataLen);

private:
    uart_port_t m_uart;
    int         m_txPin;
    int         m_rxPin;
    uint32_t    m_baud;
    bool        m_started = false;

    void     ensureStarted();
    void     startTransmission(uint8_t protectedId);
    void     sendBreak();
    uint8_t  getProtectedId(uint8_t frameId);
    uint8_t  getChecksum(uint8_t protectedId, uint8_t dataLen);
};
