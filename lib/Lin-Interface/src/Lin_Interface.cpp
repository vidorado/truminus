// Lin-Interface.cpp — patched local copy
//
// Key change vs upstream: UART driver is kept open permanently.
// Original code called HardwareSerial::begin() in every startTransmission()
// and HardwareSerial::end() after every frame, causing repeated ~2 KB
// heap alloc/free cycles that fragment the ESP32 heap until allocation fails.
// Fix: begin() once on first use (_uartStarted flag), never call end().

#include "Lin_Interface.hpp"
#include <Arduino.h>

void Lin_Interface::writeCmdWakeup()
{
    HardwareSerial::flush();
    HardwareSerial::updateBaudRate(baud >> 1);
    write(uint8_t(0x00));
    HardwareSerial::flush();
    HardwareSerial::updateBaudRate(baud);
    delay(150);
}

void Lin_Interface::writeCmdSleep()
{
    LinMessage[0] = 0;
    LinMessage[1] = 0xFF;
    LinMessage[2] = 0xFF;
    LinMessage[3] = 0xFF;
    LinMessage[4] = 0xFF;
    LinMessage[5] = 0xFF;
    LinMessage[6] = 0xFF;
    LinMessage[7] = 0xFF;
    writeFrame(0x3C, 8);
}

bool Lin_Interface::readFrame(uint8_t FrameID, uint8_t expectedlen)
{
    uint8_t ProtectedID = getProtectedID(FrameID);
    bool ChecksumValid = false;

    startTransmission(ProtectedID);
    HardwareSerial::flush();

    if (expectedlen > 0)
    {
        unsigned long wtime = millis();
        while ((millis() - wtime) < 400)
        {
          if (HardwareSerial::available() >= (expectedlen + 4)) {
            break;
          }
        }
    } else {
        delay(100);
    }

    int bytes_received = -4;
    while (HardwareSerial::available())
    {
        if (bytes_received >= (8 + 1))
            break;
        switch (bytes_received)
        {
        case -4:
        case -3:
        case -2:
        case -1:
        {
            uint8_t buffer = HardwareSerial::read();
            if (buffer == 0x00) { bytes_received = -3; }
            if (buffer == 0x55) { bytes_received = -2; }
            if (buffer == ProtectedID) { bytes_received = -1; }
            break;
        }
        default:
            LinMessage[bytes_received] = HardwareSerial::read();
        }
        bytes_received++;
    }
    uint8_t Checksum = LinMessage[bytes_received - 1];
    bytes_received--;

    HardwareSerial::flush();
    while (HardwareSerial::available()) {
        HardwareSerial::read();
        if (verboseMode > 0)
            Serial.print("additional byte discarded\n");
    }

    // PATCH: removed HardwareSerial::end() — keep UART open to avoid heap fragmentation

    ChecksumValid = (bytes_received > 0) && (0xFF == (uint8_t)(Checksum + ~getChecksum(ProtectedID, bytes_received)));

    if (verboseMode > 0)
    {
        Serial.printf(" --->>>>>> FID %02Xh        = 55|%02X|", FrameID, ProtectedID);
        for (int i = 0; i < 8; ++i)
        {
            if (i >= bytes_received) break;
            Serial.printf("%02X.", LinMessage[i]);
        }
        if (bytes_received > 0)
        {
            Serial.printf("\b|%02X", Checksum);
            Serial.printf(" bytes received %d", bytes_received);
            if (!ChecksumValid) Serial.printf(" Checksum failed");
        } else {
            Serial.printf(" no bytes received");
        }
        Serial.println();
    }

    return ChecksumValid;
}

void Lin_Interface::writeFrame(uint8_t FrameID, uint8_t dataLen)
{
    uint8_t ProtectedID = getProtectedID(FrameID);
    uint8_t cksum = getChecksum(ProtectedID, dataLen);

    startTransmission(ProtectedID);

    for (int i = 0; i < dataLen; ++i)
        HardwareSerial::write(LinMessage[i]);
    HardwareSerial::write(cksum);

    delay(20);

    if (HardwareSerial::available()) HardwareSerial::read();
    uint8_t RX_Sync = 0x00;
    if (HardwareSerial::available()) RX_Sync = HardwareSerial::read();
    uint8_t RX_ProtectedID = 0x00;
    if (HardwareSerial::available()) RX_ProtectedID = HardwareSerial::read();

    bool moreData = false;
    int bytes_received = 0;
    while (HardwareSerial::available())
    {
        if (bytes_received >= 8 + 1 + 4) { moreData = true; break; }
        LinMessage[bytes_received] = HardwareSerial::read();
        bytes_received++;
    }
    uint8_t Checksum_received = LinMessage[bytes_received - 1];
    bytes_received--;

    uint8_t ChkSumCalc = getChecksum(RX_ProtectedID, bytes_received);

    if (verboseMode > 0)
    {
        Serial.printf(" <<<<<<--- FID %02Xh (%02X)   = %02X|%02X|", FrameID, ProtectedID, RX_Sync, RX_ProtectedID);
        for (int i = 0; i < 8 + 1 + 4; ++i)
        {
            if (i >= bytes_received) break;
            Serial.printf("%02X ", LinMessage[i]);
        }
        Serial.printf("\b|%02X", Checksum_received);
        if (Checksum_received != ChkSumCalc)
            Serial.printf("\b != ChkSum calc %02Xh| TX %02Xh ", ChkSumCalc, cksum);
        if (moreData) Serial.print("more Bytes available");
        Serial.println();
    }
}

void Lin_Interface::writeFrameClassic(uint8_t FrameID, uint8_t dataLen)
{
    uint8_t ProtectedID = getProtectedID(FrameID);
    uint8_t cksum = getChecksum(0x00, dataLen);
    startTransmission(ProtectedID);
    for (int i = 0; i < dataLen; ++i)
        HardwareSerial::write(LinMessage[i]);
    HardwareSerial::write(cksum);
    HardwareSerial::flush();
    // PATCH: removed HardwareSerial::end()
}

void Lin_Interface::writeFrameClassicNoChecksum(uint8_t FrameID, uint8_t dataLen)
{
    uint8_t ProtectedID = getProtectedID(FrameID);
    startTransmission(ProtectedID);
    for (int i = 0; i < dataLen; ++i)
        HardwareSerial::write(LinMessage[i]);
    HardwareSerial::flush();
    // PATCH: removed HardwareSerial::end()
}

void Lin_Interface::startTransmission(uint8_t ProtectedID)
{
    if (!_uartStarted) {
        // Initialize UART once; keep it open for the lifetime of the task.
        if (rxPin < 0 && txPin < 0)
            HardwareSerial::begin(baud, SERIAL_8N1);
        else
            HardwareSerial::begin(baud, SERIAL_8N1, rxPin, txPin);
        _uartStarted = true;
    } else {
        // Ensure we're at normal baud rate (writeBreak may have left it at half-rate on failure)
        HardwareSerial::updateBaudRate(baud);
    }
    writeBreak();
    HardwareSerial::write(0x55);
    HardwareSerial::write(ProtectedID);
}

size_t Lin_Interface::writeBreak()
{
    HardwareSerial::flush();
    HardwareSerial::updateBaudRate(baud >> 1);
    size_t ret = HardwareSerial::write(uint8_t(0x00));
    HardwareSerial::flush();
    HardwareSerial::updateBaudRate(baud);
    return ret;
}

uint8_t Lin_Interface::getProtectedID(uint8_t FrameID)
{
    uint8_t p0 = bitRead(FrameID, 0) ^ bitRead(FrameID, 1) ^ bitRead(FrameID, 2) ^ bitRead(FrameID, 4);
    uint8_t p1 = ~(bitRead(FrameID, 1) ^ bitRead(FrameID, 3) ^ bitRead(FrameID, 4) ^ bitRead(FrameID, 5));
    return ((p1 << 7) | (p0 << 6) | (FrameID & 0x3F));
}

uint8_t Lin_Interface::getChecksum(uint8_t ProtectedID, uint8_t dataLen)
{
    uint16_t sum = ProtectedID;
    if ((sum & 0x3F) >= 0x3C) sum = 0x00;
    while (dataLen-- > 0)
        sum += LinMessage[dataLen];
    while (sum >> 8)
        sum = (sum & 0xFF) + (sum >> 8);
    return (~sum);
}
