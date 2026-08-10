#include "truma_lin.hpp"
#include "lin_driver.hpp"
#include "lin_codec.hpp"
#include "lin_frames.hpp"
#include "p4display.hpp"
#include "p4_ota.hpp"
#include "mode_controller.hpp"
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
// The stateless value codec (encodeTempKelvinX10, parseF21*, parseF22*) lives
// in lin_codec.{hpp,cpp}; the frame-field bit-packing lives in the IDF-free
// lin_frames.{hpp,cpp} module so it is host-tested against the real code. The
// wrappers below just bind those encoders to the shared frame buffers.
void f20_setRoomSetpoint(double celsius)              { linF20Room(celsius, f20.data); }
void f20_setWaterSetpoint(double celsius)             { linF20Water(celsius, f20.data); }
void f20_setFanAndWater(uint8_t pumpOrFan, uint8_t w) { linF20FanWater(pumpOrFan, w, f20.data); }
void f05_setEnergySelection(int energyIdx)           { linF05Energy(energyIdx, f05.data); }
void f06_setPowerLimit(int energyIdx)                { linF06PowerLimit(energyIdx, f06.data); }
void f07_setPumpOrFan(uint8_t pumpOrFan)             { linF07PumpOrFan(pumpOrFan, f07.data); }

// Frame 0x21/0x22 parsers (parseF21RoomTemp/parseF21WaterTemp/
// parseF22WaterHeating) now live in lin_codec.{hpp,cpp}.

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
    // Byte 3 MUST be 0x23. It is the sub-function selector, and it is the only
    // value that yields a real error report: with 0x00 the Combi answers
    // `01 06 F2 <our own D2..D5> 22` — a positive RSID (0xF2) carrying an echo
    // of the request instead of a fault, which lands as the nonexistent class
    // 0x46 / code 0x20 and is then dropped by trumaClassKnown(). The effect is
    // a unit that can be locked out while every UI shows no fault at all.
    // Verified on the bench: 0x23 returns `01 06 F2 02 06 36 FB 26` →
    // class 0x06 (hard lockout) / code 0x36 (54, flame interruption).
    masterTx[3] = 0x23;
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

// ── Task ──────────────────────────────────────────────────────────────────

LinDriver* g_lin = nullptr;
bool       g_started = false;

#ifdef LIN_SNIFF_ONLY
// Passive sniffer — assumes a real LIN master (e.g. an original CP-Plus
// connected via RJ12 splitter) is driving the bus.  Dumps frames as they
// appear, deduping repeats so the monitor stays readable.
//
// Bypasses LinDriver entirely: configures UART_NUM_1 with RX only so our
// TX can't fight the real master.  We assume RX is wired to GPIO26.
void lin_task(void*) {
    ESP_LOGW(TAG, "LIN sniffer running (LIN_SNIFF_ONLY — RX-only on GPIO26)");

    uart_config_t cfg = {};
    cfg.baud_rate  = 9600;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_XTAL;
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 1024, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &cfg));
    // Only set RX; TX stays unrouted so GPIO27 floats high via external pull-up.
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, UART_PIN_NO_CHANGE, 26,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_set_pull_mode((gpio_num_t)26, GPIO_PULLUP_ONLY);

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
#else
void lin_task(void*) {
    ESP_LOGI(TAG, "LIN task running on core %d", xPortGetCoreID());

#ifdef LIN_SELFTEST
    // One-shot loopback diagnostic: sweeps baud rates transmitting 0x55 and
    // dumps the half-duplex echo.  Use it to separate UART/baud problems from
    // physical-layer ones (transceiver pull-ups, bus slew).  Build with
    // -DLIN_SELFTEST to enable; off by default.
    g_lin->selfTestLoopback();
#endif

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

        TrumaLinSetpoints setpoints = derive_mode(cs);

        // 2. Encode setpoint frames.
        encodeTempKelvinX10(-273.0,            &f02.data[0]);   // no simulated temp
        encodeTempKelvinX10(setpoints.roomSp,  &f03.data[0]);
        encodeTempKelvinX10(setpoints.waterSp, &f04.data[0]);
        f05_setEnergySelection(cs.energyIdx);
        f06_setPowerLimit(cs.energyIdx);
        f07_setPumpOrFan(setpoints.pumpOrFan);
        f20_setRoomSetpoint(setpoints.roomSp);
        f20_setWaterSetpoint(setpoints.waterSp);
        f20_setFanAndWater(setpoints.pumpOrFan, (setpoints.waterSp > 0.0) ? 1 : 0);

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
        p4OtaBeat(P4OTA_BEAT_LIN);   // liveness for the post-OTA self-test
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
#if defined(LIN_SNIFF_ONLY)
    // Sniff mode configures UART directly inside lin_task — no LinDriver needed.
    (void)tx_pin; (void)rx_pin;
#else
    // Truma Combi D LIN bus runs at 9600 baud, not the 19200 LIN default.
    g_lin = new LinDriver(UART_NUM_1, tx_pin, rx_pin, 9600);
    g_lin->verboseMode = 1;   // dump bytes when a read returns no valid frame
#endif

    xTaskCreatePinnedToCore(lin_task, "truma_lin", 4096, nullptr, 5, nullptr, 0);
    ESP_LOGI(TAG, "scheduler started on UART1 TX=%d RX=%d @9600", tx_pin, rx_pin);
}

bool trumaLinGetSnapshot(TrumaLinSnapshot& out) {
    if (!g_lock || xSemaphoreTake(g_lock, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    out = g_snap;
    xSemaphoreGive(g_lock);
    return true;
}
