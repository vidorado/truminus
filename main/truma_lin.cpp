#include "truma_lin.hpp"
#include "lin_driver.hpp"
#include "p4display.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cmath>
#include <cstring>
#include <endian.h>
#include "driver/uart.h"
#include "driver/gpio.h"

static const char* TAG = "truma_lin";

namespace {

// ── Frame byte buffers ────────────────────────────────────────────────────
//
// One 8-byte buffer per frame the scheduler emits/reads.  Encoding helpers
// below mutate these in place; the LIN task copies the buffer into the
// driver's `LinMessage` field right before each write/read.

struct FrameBuf { uint8_t fid; uint8_t data[8]; };

// Setpoint frames (slave responses written by the master).
FrameBuf f02 = { 0x02, {0,0,0,0,0,0,0,0} };  // simulated room temperature
FrameBuf f03 = { 0x03, {0,0,0,0,0,0,0,0} };  // room setpoint
FrameBuf f04 = { 0x04, {0,0,0,0,0,0,0,0} };  // water setpoint
FrameBuf f05 = { 0x05, {0,0,0,0,0,0,0,0} };  // energy priority
FrameBuf f06 = { 0x06, {0,0,0,0,0,0,0,0} };  // power limit
FrameBuf f07 = { 0x07, {0x10|0xE0, 0xFE, 0,0,0,0,0,0} };  // pump/fan
FrameBuf f20 = { 0x20, {0xAA,0xAA,0xAA,0xFA,0x00,0x01,0xE0,0x0F} };  // CP-Plus D control

// Master request scratch (re-encoded each cycle from m_state).
uint8_t masterTx[8];
uint8_t masterRx[8];

// ── Encoding helpers (ported from trumaframes.cpp) ────────────────────────

void encodeTempKelvinX10(double celsius, uint8_t* dest /*2 bytes*/) {
    uint16_t raw = (uint16_t)htole16((uint16_t)lround((celsius + 273.0) * 10.0));
    memcpy(dest, &raw, 2);
}

// Frame 0x20 bytes 0-1: room setpoint as 12-bit K×10 + flags nibble 0xA.
// celsius outside [5,30] → 0xAA 0xAA (heating-off sentinel from CP-Plus capture).
void f20_setRoomSetpoint(double celsius) {
    if (celsius < 5.0 || celsius > 30.0) {
        f20.data[0] = 0xAA;
        f20.data[1] = 0xAA;
        return;
    }
    uint16_t raw = (uint16_t)lround((celsius + 273.0) * 10.0);
    f20.data[0] = raw & 0xFF;
    f20.data[1] = 0xA0 | ((raw >> 8) & 0x0F);
}

// Frame 0x20 byte 2: water setpoint as K×10 >> 4.  celsius<1 → 0xAA.
void f20_setWaterSetpoint(double celsius) {
    if (celsius < 1.0) { f20.data[2] = 0xAA; return; }
    uint16_t raw = (uint16_t)lround((celsius + 273.0) * 10.0);
    f20.data[2] = (uint8_t)(raw >> 4);
}

// Frame 0x20 byte 5: high nibble = heating/fan mode, low nibble = water mode.
// pumpOrFan==1 → 0xB (eco heat), ==2 → 0xD (high heat), >=0x10 → fan level.
void f20_setFanAndWater(uint8_t pumpOrFan, uint8_t waterMode) {
    uint8_t fanNibble;
    if (pumpOrFan == 1) {
        fanNibble = 0xB;
    } else if (pumpOrFan == 2) {
        fanNibble = 0xD;
    } else {
        uint8_t level = (pumpOrFan >= 0x10) ? (pumpOrFan & 0x0F) : pumpOrFan;
        fanNibble = level;
    }
    f20.data[5] = (fanNibble << 4) | (waterMode & 0x0F);
}

void f05_setEnergySelection(int energyIdx) {
    // P4 indices map 1:1 to legacy TEnergySelection (Gas, Mix900, Mix1800, Elec900, Elec1800).
    // Priority codes: 1=EpFuel, 2=EpBothPrioElectro, 3=EpBothPrioFuel.
    static const uint8_t priorities[5] = { 1, 3, 3, 2, 2 };
    if (energyIdx < 0 || energyIdx > 4) energyIdx = 0;
    f05.data[0] = priorities[energyIdx];
}

void f06_setPowerLimit(int energyIdx) {
    static const uint16_t limits[5] = { 0, 900, 1800, 900, 1800 };
    if (energyIdx < 0 || energyIdx > 4) energyIdx = 0;
    uint16_t v = (uint16_t)htole16(limits[energyIdx]);
    memcpy(&f06.data[0], &v, 2);
}

void f07_setPumpOrFan(uint8_t pumpOrFan) {
    f07.data[0] = pumpOrFan | 0xE0;
    f07.data[1] = 0xFE;
}

// ── Frame 0x21 parse ──────────────────────────────────────────────────────
//
// byte 0   = Kelvin×10 LSB (bits 7:0 of room temp 12-bit value)
// byte 1   = bits 3:0 → room K×10 bits 11:8, bits 7:4 → water K×10 bits 3:0
// byte 2   = water K×10 bits 11:4
// Range gates: room ∈ [0,50] °C, water ∈ [0,100] °C — anything else → -273.
double parseF21RoomTemp(const uint8_t* d) {
    uint16_t raw = (uint16_t)d[0] | ((uint16_t)(d[1] & 0x0F) << 8);
    double t = raw / 10.0 - 273.0;
    return (t < 0.0 || t > 50.0) ? NAN : t;
}
double parseF21WaterTemp(const uint8_t* d) {
    uint16_t raw = (uint16_t)(d[1] >> 4) | ((uint16_t)d[2] << 4);
    double t = raw / 10.0 - 273.0;
    return (t < 0.0 || t > 100.0) ? NAN : t;
}

// 0x22 byte1: 0x40/0x50 = water heating, 0x00=off, 0xD0=idle.
bool parseF22WaterHeating(const uint8_t* d) {
    return (d[1] & 0xC0) == 0x40;
}

// ── Master frame helpers (transport over 0x3C / 0x3D) ─────────────────────

void encodeMasterOnOff(bool on) {
    masterTx[0] = 0x01;   // NAD
    masterTx[1] = 0x06;   // LEN (single frame)
    masterTx[2] = 0xB8;   // SID = OnOff
    masterTx[3] = 0x20;
    masterTx[4] = 0x03;
    masterTx[5] = on ? 0x01 : 0x00;
    masterTx[6] = 0x00;
    masterTx[7] = 0xFF;
}
void encodeMasterGetError() {
    masterTx[0] = 0x7F;   // NAD (broadcast variant used by CP-Plus)
    masterTx[1] = 0x06;
    masterTx[2] = 0xB2;   // SID = GetErrorInfo
    // Byte 3 = 0x00 confirmed by sniff of a real CP-Plus D on the bus.
    // The truma-protocol skill (and the legacy port it inherited) had 0x23
    // here, which the Truma might silently reject — possibly the cause of
    // the spurious E545 errors users have seen.
    masterTx[3] = 0x00;
    masterTx[4] = 0x17;
    masterTx[5] = 0x46;
    masterTx[6] = 0x20;
    masterTx[7] = 0x03;
}

// ── Shared snapshot ────────────────────────────────────────────────────────

SemaphoreHandle_t g_lock = nullptr;
TrumaLinSnapshot  g_snap = { false, NAN, NAN, false, 0, 0, 0, 0 };

void publish_snapshot(const TrumaLinSnapshot& s) {
    if (g_lock && xSemaphoreTake(g_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_snap = s;
        xSemaphoreGive(g_lock);
    }
}

// ── Operating-mode derivation (port of legacy linBusTask logic) ───────────
//
// Maps the LCD's (heatingOn, fanMode 0/1/2/3..12, boilerMode 0..3, roomSetpoint)
// to the byte-level values the Truma expects:
//   *pumpOrFan: 0x10..0x1A for fan-only modes, 1=eco-heat, 2=high-heat
//   *roomSp:    0 when heating off, else roomSetpoint
//   *waterSp:   from boilerMode (0=off, 1=40°C, 2/3=60°C)
//
// Legacy fan int encoding was 0=off / -1=eco / -2=high / 1..10=level.
// p4display uses 0=off / 1=eco / 2=high / 3..12=level — translated here.
void derive_mode(const P4ControlState& cs,
                 uint8_t& pumpOrFan, double& roomSp, double& waterSp)
{
    int legacyFan;  // 0=off, -1=eco, -2=high, 1..10=level
    if      (cs.fanMode == 0) legacyFan = 0;
    else if (cs.fanMode == 1) legacyFan = -1;
    else if (cs.fanMode == 2) legacyFan = -2;
    else                      legacyFan = cs.fanMode - 2;   // 1..10

    if (!cs.heatingOn) {
        // Heating off → fan-only modes use the 0x10 base; eco/high stay
        // mapped to 0x11 / 0x12 (Truma still expects those even without heat).
        if      (legacyFan > 0)   pumpOrFan = 0x10 | legacyFan;
        else if (legacyFan == -1) pumpOrFan = 0x11;
        else if (legacyFan == -2) pumpOrFan = 0x12;
        else                      pumpOrFan = 0x10;
        roomSp = 0.0;
    } else {
        roomSp = cs.roomSetpoint;
        // Heating on + numeric fan level: legacy code forced level→eco/high.
        // Keep that compromise — Truma rejects level numbers in heat mode.
        pumpOrFan = (legacyFan == -2) ? 2 : 1;
    }

    switch (cs.boilerMode) {
        case 1:  waterSp = 40.0; break;
        case 2:  waterSp = 60.0; break;
        case 3:  waterSp = 60.0; break;   // boost (no waterboost cycle in MVP)
        default: waterSp = 0.0;  break;
    }
}

// ── Task ──────────────────────────────────────────────────────────────────

LinDriver* g_lin = nullptr;
bool       g_started = false;

#ifdef LIN_SNIFF_ONLY
// Passive sniffer — assumes a real LIN master (e.g. an original CP-Plus
// connected via RJ12 splitter) is driving the bus.  Dumps frames as they
// appear, deduping repeats so the monitor stays readable.
//
// Bypasses LinDriver entirely: configures UART_NUM_1 with RX only so our
// TX can't fight the real master.  We assume RX is wired to GPIO27.
void lin_task(void*) {
    ESP_LOGW(TAG, "LIN sniffer running (LIN_SNIFF_ONLY — RX-only on GPIO27)");

    uart_config_t cfg = {};
    cfg.baud_rate  = 9600;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_XTAL;
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 1024, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &cfg));
    // Only set RX; TX stays unrouted so GPIO26 floats high via external pull-up.
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, UART_PIN_NO_CHANGE, 27,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_set_pull_mode((gpio_num_t)27, GPIO_PULLUP_ONLY);

    enum St { S_IDLE, S_GOT_BREAK, S_GOT_SYNC, S_IN_FRAME };
    St st = S_IDLE;
    uint8_t pid = 0; uint8_t buf[12]; int n = 0;
    uint32_t lastByteMs = 0;
    auto nowMs = []() { return (uint32_t)(esp_timer_get_time() / 1000ULL); };

    // Per-PID dedup: only print when bytes differ from the last sample.
    struct Slot { uint8_t pid; uint8_t data[9]; bool seen; };
    Slot hist[32] = {}; int histN = 0;

    // Heartbeat: count raw bytes received and dump last few every 2 s.
    uint32_t hbCnt = 0; uint32_t hbLastMs = 0;
    uint8_t  hbTail[16] = {0}; int hbTailIdx = 0;

    for (;;) {
        uint8_t b;
        int r = uart_read_bytes(UART_NUM_1, &b, 1, pdMS_TO_TICKS(20));
        uint32_t now = nowMs();
        if (now - hbLastMs > 2000) {
            hbLastMs = now;
            char hex[64]; int o = 0;
            for (int i = 0; i < 16; i++) o += snprintf(hex+o, sizeof(hex)-o, "%02X ", hbTail[i]);
            ESP_LOGI(TAG, "sniff hb: %lu bytes total, last16: %s",
                     (unsigned long)hbCnt, hex);
        }
        if (r != 1) {
            // Inter-frame timeout: flush partial frame if any.
            if (st == S_IN_FRAME && n > 0 && (now - lastByteMs) > 8) {
                ESP_LOGI(TAG, "[%02X] partial (%d B) TIMEOUT", pid, n);
                st = S_IDLE; n = 0;
            }
            continue;
        }
        hbCnt++;
        hbTail[hbTailIdx] = b; hbTailIdx = (hbTailIdx + 1) % 16;
        uint32_t t = now;
        if (st == S_IN_FRAME && n > 0 && (t - lastByteMs) > 8) {
            ESP_LOGI(TAG, "[%02X] partial (%d B) TIMEOUT", pid, n);
            st = S_IDLE; n = 0;
        }
        lastByteMs = t;
        switch (st) {
            case S_IDLE:
            case S_GOT_BREAK:
                if      (b == 0x00)                            st = S_GOT_BREAK;
                else if (b == 0x55 && st == S_GOT_BREAK)       st = S_GOT_SYNC;
                else                                            st = S_IDLE;
                break;
            case S_GOT_SYNC:
                pid = b; n = 0; st = S_IN_FRAME;
                break;
            case S_IN_FRAME:
                if (n < 12) buf[n++] = b;
                if (n == 9) {
                    int slot = -1;
                    for (int i = 0; i < histN; i++) if (hist[i].pid == pid) { slot = i; break; }
                    bool isNew = (slot < 0);
                    if (isNew && histN < 32) { slot = histN++; hist[slot].pid = pid; }
                    if (slot >= 0) {
                        bool changed = isNew || !hist[slot].seen ||
                                       memcmp(hist[slot].data, buf, 9) != 0;
                        if (changed) {
                            char hex[64]; int o = 0;
                            for (int i = 0; i < 8; i++)
                                o += snprintf(hex+o, sizeof(hex)-o, "%02X ", buf[i]);
                            ESP_LOGI(TAG, "[%02X] %s cs=%02X", pid, hex, buf[8]);
                            hist[slot].seen = true;
                            memcpy(hist[slot].data, buf, 9);
                        }
                    }
                    st = S_IDLE; n = 0;
                }
                break;
        }
    }
}
#elif defined(LIN_SLAVE_EMU)
// Slave emulator — pretends to be a Truma Combi D for bench testing.  Used
// when the real boiler is unreachable: connect a working LIN master (e.g.
// the C5 board acting as CP-Plus) to the P4 transceiver and watch the C5
// monitor.  If readFrame(0x21) succeeds on the C5 side, the P4's TJA1021
// is good.
//
// Wire layout: GPIO26 = TX, GPIO27 = RX, UART_NUM_1 @ 9600 8N1, XTAL clock.
// We're the only slave on the bus, so half-duplex collision avoidance is
// trivial: only transmit while in S_GOT_SYNC after a PID we own, never
// while the master is sending the header.
void lin_task(void*) {
    ESP_LOGW(TAG, "LIN slave emu running (LIN_SLAVE_EMU — TX=GPIO26 RX=GPIO27)");

    uart_config_t cfg = {};
    cfg.baud_rate  = 9600;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_XTAL;
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 1024, 256, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, 26, 27,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_set_pull_mode((gpio_num_t)27, GPIO_PULLUP_ONLY);

    auto nowMs = []() { return (uint32_t)(esp_timer_get_time() / 1000ULL); };

    // Enhanced LIN checksum: sum of PID + data bytes, end-around carry, inverted.
    // For PID & 0x3F >= 0x3C use classic (PID excluded).
    auto linChecksum = [](uint8_t pid, const uint8_t* data, int len) -> uint8_t {
        uint16_t sum = ((pid & 0x3F) >= 0x3C) ? 0 : pid;
        for (int i = 0; i < len; i++) sum += data[i];
        while (sum >> 8) sum = (sum & 0xFF) + (sum >> 8);
        return (uint8_t)(~sum);
    };

    auto sendResponse = [&](uint8_t pid, const uint8_t* data) {
        uint8_t pkt[9];
        memcpy(pkt, data, 8);
        pkt[8] = linChecksum(pid, data, 8);
        uart_write_bytes(UART_NUM_1, pkt, 9);
        uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(20));
    };

    // Canned slave data.
    auto encF21 = [](double room, double water, uint8_t* out) {
        uint16_t r = (uint16_t)lround((room  + 273.0) * 10.0);
        uint16_t w = (uint16_t)lround((water + 273.0) * 10.0);
        out[0] = r & 0xFF;
        out[1] = ((r >> 8) & 0x0F) | ((w & 0x0F) << 4);
        out[2] = (uint8_t)((w >> 4) & 0xFF);
        out[3] = 0; out[4] = 0; out[5] = 0; out[6] = 0; out[7] = 0;
    };
    uint8_t resp21[8];
    encF21(22.0, 45.0, resp21);
    uint8_t resp22[8] = { 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    // 0x3D: ack of last 0x3C — copy of master's request with SID + 0x40.
    uint8_t resp3D[8] = { 0x01, 0x06, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF };
    bool have3C = false;

    // Pre-computed protected IDs (master sends these on the wire).
    constexpr uint8_t PID_21 = 0x61;  // frame 0x21 → PID 0x61
    constexpr uint8_t PID_22 = 0xE2;  // frame 0x22 → PID 0xE2
    constexpr uint8_t PID_3C = 0x3C;  // diag frames use classic PID = ID
    constexpr uint8_t PID_3D = 0x7D;  // frame 0x3D → PID 0x7D

    enum St { S_IDLE, S_GOT_BREAK, S_GOT_SYNC, S_IN_DATA };
    St st = S_IDLE;
    uint8_t pid = 0; uint8_t buf[12]; int n = 0; int expect = 0;
    uint32_t lastByteMs = 0;

    uint32_t hbLastMs = 0;
    uint32_t replies21 = 0, replies22 = 0, replies3D = 0, headersSeen = 0;

    for (;;) {
        uint8_t b;
        int r = uart_read_bytes(UART_NUM_1, &b, 1, pdMS_TO_TICKS(20));
        uint32_t now = nowMs();
        if (now - hbLastMs > 2000) {
            hbLastMs = now;
            ESP_LOGI(TAG, "slave hb: headers=%lu  replies 0x21=%lu 0x22=%lu 0x3D=%lu",
                     (unsigned long)headersSeen,
                     (unsigned long)replies21, (unsigned long)replies22,
                     (unsigned long)replies3D);
        }
        if (r != 1) {
            if (st == S_IN_DATA && n > 0 && (now - lastByteMs) > 8) {
                st = S_IDLE; n = 0;
            }
            continue;
        }
        if (st == S_IN_DATA && n > 0 && (now - lastByteMs) > 8) {
            st = S_IDLE; n = 0;
        }
        lastByteMs = now;
        switch (st) {
            case S_IDLE:
            case S_GOT_BREAK:
                if      (b == 0x00)                            st = S_GOT_BREAK;
                else if (b == 0x55 && st == S_GOT_BREAK)       st = S_GOT_SYNC;
                else                                            st = S_IDLE;
                break;
            case S_GOT_SYNC: {
                pid = b; n = 0; headersSeen++;
                // PID drives the action: respond, swallow, or ignore.
                if (pid == PID_21) {
                    sendResponse(pid, resp21);
                    replies21++;
                    st = S_IDLE;
                } else if (pid == PID_22) {
                    sendResponse(pid, resp22);
                    replies22++;
                    st = S_IDLE;
                } else if (pid == PID_3D) {
                    sendResponse(pid, resp3D);
                    replies3D++;
                    st = S_IDLE;
                } else {
                    // Master is going to write 8 data bytes + checksum.  Swallow
                    // them so they don't poison the next frame's parser.
                    expect = 9;
                    st = S_IN_DATA;
                }
                break;
            }
            case S_IN_DATA:
                if (n < 12) buf[n++] = b;
                if (n == expect) {
                    // If this was a 0x3C from the master, snapshot it so the
                    // next 0x3D reply contains the matching SID + 0x40 ack.
                    if (pid == PID_3C && expect == 9) {
                        memcpy(resp3D, buf, 8);
                        resp3D[2] = (uint8_t)(buf[2] + 0x40);
                        have3C = true;
                    }
                    (void)have3C;
                    st = S_IDLE; n = 0;
                }
                break;
        }
    }
}
#else
void lin_task(void*) {
    ESP_LOGI(TAG, "LIN task running on core %d", xPortGetCoreID());

    // Wakeup pulse before the first transaction.  Matches the legacy driver:
    // writes a single 0x00 at half baud, then waits 150 ms.
    g_lin->writeCmdWakeup();

    enum MasterTurn { MT_ONOFF, MT_GET_ERR };
    MasterTurn masterTurn = MT_ONOFF;

    bool onState = false;           // last commanded on/off
    uint32_t lastActivityMs = 0;    // for the 20 s off-delay
    auto nowMs = []() { return (uint32_t)(esp_timer_get_time() / 1000ULL); };

    TrumaLinSnapshot snap = {};
    // NaN = "no data yet"; p4display checks isnan() to render "--".
    snap.roomTemp  = NAN;
    snap.waterTemp = NAN;
    uint32_t lastLinOkMs = 0;

    // Diagnostic counters — dumped every 5 s so you can see slave activity
    // even when no frame fully validates (checksum, length, …).
    uint32_t cyclesTotal = 0;
    uint32_t okF21 = 0, okF22 = 0, okF3D = 0;
    uint32_t lastDumpMs = 0;

    for (;;) {
        // 1. Pull setpoints from the LCD/WS state.
        P4ControlState cs;
        p4GetControlState(cs);

        uint8_t pumpOrFan;
        double  roomSp, waterSp;
        derive_mode(cs, pumpOrFan, roomSp, waterSp);

        // 2. Encode setpoint frames.
        encodeTempKelvinX10(-273.0,            &f02.data[0]);   // no simulated temp
        encodeTempKelvinX10(roomSp,            &f03.data[0]);
        encodeTempKelvinX10(waterSp,           &f04.data[0]);
        f05_setEnergySelection(cs.energyIdx);
        f06_setPowerLimit(cs.energyIdx);
        f07_setPumpOrFan(pumpOrFan);
        f20_setRoomSetpoint(roomSp);
        f20_setWaterSetpoint(waterSp);
        f20_setFanAndWater(pumpOrFan, (waterSp > 0.0) ? 1 : 0);

        // 3. Drive the on/off state machine.
        bool active = cs.heatingOn || cs.boilerMode > 0 || cs.fanMode > 0;
        if (active) {
            onState = true;
            lastActivityMs = nowMs();
        } else if (onState && (nowMs() - lastActivityMs) > 20000) {
            onState = false;
        }

        // 4. Read frames 0x21 and 0x22.
        bool gotF21 = g_lin->readFrame(0x21, 8);
        if (gotF21) {
            snap.roomTemp  = (float)parseF21RoomTemp(g_lin->LinMessage);
            snap.waterTemp = (float)parseF21WaterTemp(g_lin->LinMessage);
            lastLinOkMs = nowMs();
            okF21++;
        }
        bool gotF22 = g_lin->readFrame(0x22, 8);
        if (gotF22) {
            snap.waterHeating = parseF22WaterHeating(g_lin->LinMessage);
            okF22++;
        }
        snap.linOk = (nowMs() - lastLinOkMs) < 5000;

        // 5. Write all 7 setpoint frames + the control frame.
        const FrameBuf* writes[] = { &f02, &f03, &f04, &f05, &f06, &f07, &f20 };
        for (auto fb : writes) {
            memcpy(g_lin->LinMessage, fb->data, 8);
            g_lin->writeFrame(fb->fid, 8);
        }

        // 6. One master request per cycle, alternating OnOff and GetErrorInfo.
        //    Send 0x3C with our request, then read 0x3D and parse the reply.
        if (masterTurn == MT_ONOFF) {
            encodeMasterOnOff(onState);
            masterTurn = MT_GET_ERR;
        } else {
            encodeMasterGetError();
            masterTurn = MT_ONOFF;
        }
        memcpy(g_lin->LinMessage, masterTx, 8);
        g_lin->writeFrame(0x3C, 8);
        if (g_lin->readFrame(0x3D, 8)) {
            memcpy(masterRx, g_lin->LinMessage, 8);
            okF3D++;
            // Reply SID is request SID + 0x40 (LIN diagnostic ack pattern).
            if (masterRx[2] == (masterTx[2] + 0x40)) {
                if (masterTx[2] == 0xB8) {        // OnOff reply
                    snap.requestedState = masterRx[3];
                    snap.currentState   = masterRx[4];
                } else if (masterTx[2] == 0xB2) { // GetErrorInfo reply
                    snap.errClass = masterRx[4];
                    snap.errCode  = masterRx[5];
                }
            }
        }

        publish_snapshot(snap);
        cyclesTotal++;

        uint32_t t = nowMs();
        if (t - lastDumpMs > 5000) {
            ESP_LOGI(TAG, "cycles=%lu  rxBytes=%lu  reads ok: 0x21=%lu 0x22=%lu 0x3D=%lu  linOk=%d  room=%.1f water=%.1f",
                     (unsigned long)cyclesTotal,
                     (unsigned long)g_lin->rxBytesTotal,
                     (unsigned long)okF21, (unsigned long)okF22, (unsigned long)okF3D,
                     snap.linOk, snap.roomTemp, snap.waterTemp);
            lastDumpMs = t;
        }

        // Small delay to let other tasks run; the loop itself spends most of
        // its time blocked on UART reads.
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
#endif  // LIN_SNIFF_ONLY

}  // namespace

// ── Public API ────────────────────────────────────────────────────────────

void trumaLinStart(int tx_pin, int rx_pin) {
    if (g_started) return;
    g_started = true;

    g_lock = xSemaphoreCreateMutex();
#if defined(LIN_SNIFF_ONLY) || defined(LIN_SLAVE_EMU)
    // Sniff / slave-emu modes configure UART directly inside lin_task —
    // no LinDriver needed.
    (void)tx_pin; (void)rx_pin;
#else
    // Truma Combi D LIN bus runs at 9600 baud, not the 19200 LIN default.
    g_lin = new LinDriver(UART_NUM_1, tx_pin, rx_pin, 9600);
    g_lin->verboseMode = 1;   // dump bytes when a read returns no valid frame
#endif

    xTaskCreatePinnedToCore(lin_task, "truma_lin", 4096, nullptr, 5, nullptr, 0);
    ESP_LOGI(TAG, "scheduler started on UART1 TX=%d RX=%d @9600", tx_pin, rx_pin);
}

void trumaLinGetSnapshot(TrumaLinSnapshot& out) {
    if (g_lock && xSemaphoreTake(g_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        out = g_snap;
        xSemaphoreGive(g_lock);
    } else {
        out = {};
        out.roomTemp  = NAN;
        out.waterTemp = NAN;
    }
}
