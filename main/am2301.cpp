#include "am2301.hpp"
#include "logs.hpp"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"

// 1 MHz resolution → 1 tick = 1 µs, which comfortably resolves the AM2301
// bit timings (26 µs "0" high vs 70 µs "1" high).
#define AM2301_RESOLUTION_HZ   1000000
#define AM2301_POLL_MS         30000      // steady-state cadence between reads
#define AM2301_RETRY_MS        2500       // quick retry after a failed read
                                          // (sensor spec: >= 2 s between reads)
#define AM2301_START_LOW_US    1200       // host start pulse (spec 0.8–20 ms)
// P4 RMT memory is 48 words/channel (SOC_RMT_MEM_WORDS_PER_CHANNEL); a full
// AM2301 frame (start + handshake + 40 bits ≈ 43 symbols) fits in one block.
#define AM2301_MEM_SYMBOLS     48
#define AM2301_MAX_SYMBOLS     48         // user RX buffer depth

// A captured "1" bit has ~70 µs high; "0" ~26 µs.  Threshold halfway.
#define AM2301_BIT_THRESH_US   50

static const char* TAG = "am2301";

static rmt_channel_handle_t s_rx_chan  = nullptr;
static rmt_channel_handle_t s_tx_chan  = nullptr;
static rmt_encoder_handle_t s_copy_enc = nullptr;
static QueueHandle_t        s_rx_queue = nullptr;   // rmt_rx_done_event_data_t

static Am2301Data           s_data = { NAN, NAN, false, 0 };
static portMUX_TYPE         s_mux  = portMUX_INITIALIZER_UNLOCKED;
static bool                 s_started = false;

// RX receive window: shortest valid edge (glitch filter) and longest same-
// level span before RMT calls the frame finished.  Must exceed our own
// ~1.2 ms start-low pulse (RX is armed before it and captures it), so 2 ms;
// the indefinite trailing high after the last bit (sensor releases the line)
// still exceeds it and ends reception.
static const rmt_receive_config_t s_rx_cfg = {
    .signal_range_min_ns = 1000,        // 1 µs — drop sub-µs glitches
    .signal_range_max_ns = 2000000,     // 2 ms held level → end of frame
    .flags = {},
};

static bool IRAM_ATTR rx_done_cb(rmt_channel_handle_t,
                                 const rmt_rx_done_event_data_t* edata,
                                 void* user_ctx) {
    QueueHandle_t q = static_cast<QueueHandle_t>(user_ctx);
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(q, edata, &hp);
    return hp == pdTRUE;
}

// Decode the captured symbol stream into the 5 raw bytes.  Returns true and
// fills tempC/humidity on a valid checksum.
static bool decode(const rmt_symbol_word_t* syms, size_t n,
                   float& tempC, float& humidity) {
    // Each data bit is encoded as a low (~50 µs) followed by a high whose
    // length distinguishes 0 from 1.  In RMT symbols the high pulse is the
    // level==1 half.  The first symbol carries the sensor's 80 µs/80 µs
    // response handshake; we collect *all* high durations and keep the last
    // 40 (the data bits), which sidesteps off-by-one on the handshake.
    // Collect every high-pulse duration; the stream carries a couple of
    // leading highs (host release + sensor 80 µs handshake) before the 40
    // data bits, so we over-collect and keep the last 40 below.
    uint8_t bits[AM2301_MAX_SYMBOLS];
    int     nbits = 0;

    for (size_t i = 0; i < n && nbits < (int)sizeof(bits); i++) {
        // duration0/level0 is the first half of the symbol, duration1/level1
        // the second.  Examine whichever half is the high pulse.
        uint16_t hi_us = 0;
        if (syms[i].level0 == 1)       hi_us = syms[i].duration0;
        else if (syms[i].level1 == 1)  hi_us = syms[i].duration1;
        else                           continue;          // low-low: skip
        // The handshake's 80 µs high would read as a "1"; harmless because we
        // keep only the final 40 collected bits below.
        if (hi_us == 0) continue;
        bits[nbits++] = (hi_us > AM2301_BIT_THRESH_US) ? 1 : 0;
    }

    if (nbits < 40) {
        LOG_AM2301_PF("decode: only %d bits", nbits);
        return false;
    }
    // Keep the last 40 bits (skip any leading handshake high we may have
    // captured as an extra symbol).
    const uint8_t* b = bits + (nbits - 40);

    uint8_t raw[5] = {0};
    for (int i = 0; i < 40; i++)
        raw[i / 8] = (raw[i / 8] << 1) | b[i];

    uint8_t sum = raw[0] + raw[1] + raw[2] + raw[3];
    if (sum != raw[4]) {
        LOG_AM2301_PF("checksum: got %02X want %02X", raw[4], sum);
        return false;
    }

    humidity = ((raw[0] << 8) | raw[1]) / 10.0f;
    int16_t t = ((raw[2] & 0x7F) << 8) | raw[3];
    tempC = (raw[2] & 0x80) ? -t / 10.0f : t / 10.0f;
    return true;
}

// One full transaction: drive the start pulse, capture the response, decode.
static bool read_once(float& tempC, float& humidity) {
    rmt_symbol_word_t syms[AM2301_MAX_SYMBOLS];

    // Arm RX before the start pulse so we don't miss the sensor's response.
    esp_err_t err = rmt_receive(s_rx_chan, syms, sizeof(syms), &s_rx_cfg);
    if (err != ESP_OK) {
        LOG_AM2301_PF("rmt_receive: %s", esp_err_to_name(err));
        return false;
    }

    // Start pulse: pull the open-drain line low for ~1.2 ms, then release
    // (high-Z, the line pull-up brings it back up and the sensor takes over).
    rmt_symbol_word_t start = {};
    start.level0    = 0;  start.duration0 = AM2301_START_LOW_US;
    start.level1    = 1;  start.duration1 = 40;          // brief release
    rmt_transmit_config_t tx_cfg = {};
    tx_cfg.flags.eot_level = 1;     // leave the OD line released (high) after TX
    err = rmt_transmit(s_tx_chan, s_copy_enc, &start, sizeof(start), &tx_cfg);
    if (err != ESP_OK) {
        LOG_AM2301_PF("rmt_transmit: %s", esp_err_to_name(err));
        return false;
    }

    rmt_rx_done_event_data_t rx = {};
    if (xQueueReceive(s_rx_queue, &rx, pdMS_TO_TICKS(200)) != pdTRUE) {
        LOG_AM2301_PL("rx timeout (no sensor response — check wiring / pull-up)");
        return false;
    }
    LOG_AM2301_PF("rx %u symbols", (unsigned)rx.num_symbols);
    return decode(rx.received_symbols, rx.num_symbols, tempC, humidity);
}

static void am2301_task(void* /*arg*/) {
    // Sensor needs ~1 s to stabilise after power-on before the first read.
    vTaskDelay(pdMS_TO_TICKS(1500));

    uint16_t errStreak = 0;
    for (;;) {
        rmt_enable(s_rx_chan);
        rmt_enable(s_tx_chan);

        float t, h;
        bool ok = read_once(t, h);

        rmt_disable(s_rx_chan);
        rmt_disable(s_tx_chan);

        if (ok) {
            portENTER_CRITICAL(&s_mux);
            s_data.tempC    = t;
            s_data.humidity = h;
            s_data.valid    = true;
            s_data.lastMs   = (uint32_t)(esp_timer_get_time() / 1000);
            portEXIT_CRITICAL(&s_mux);
            errStreak = 0;
            LOG_AM2301_PF("%.1f C  %.0f %%RH", t, h);
        } else {
            errStreak++;
            // The first read after power-up commonly fails (sensor settling);
            // only flag it once it persists.
            if (errStreak > 1 && (errStreak <= 3 || (errStreak % 20) == 0))
                LOG_AM2301_PF("read failed (streak=%u)", errStreak);
        }

        // Retry quickly after a failure so a single settling miss doesn't cost
        // a whole 30 s cycle; back off to the steady cadence once we have data.
        vTaskDelay(pdMS_TO_TICKS(ok ? AM2301_POLL_MS : AM2301_RETRY_MS));
    }
}

bool am2301Start(gpio_num_t pin) {
    if (s_started) return true;

    s_rx_queue = xQueueCreate(2, sizeof(rmt_rx_done_event_data_t));
    if (!s_rx_queue) {
        ESP_LOGE(TAG, "queue alloc failed");
        return false;
    }

    // RX channel — captures the sensor response.  Binding both the RX and TX
    // channels to the same GPIO number makes the IDF 6.0 RMT driver wire them
    // in loopback automatically (no flag needed).
    rmt_rx_channel_config_t rx_cfg = {};
    rx_cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
    rx_cfg.resolution_hz     = AM2301_RESOLUTION_HZ;
    rx_cfg.mem_block_symbols = AM2301_MEM_SYMBOLS;
    rx_cfg.gpio_num          = pin;
    esp_err_t err = rmt_new_rx_channel(&rx_cfg, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rx channel: %s", esp_err_to_name(err));
        return false;
    }

    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = rx_done_cb;
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(s_rx_chan, &cbs, s_rx_queue));

    // TX channel — only used to generate the start-low pulse, on the same
    // GPIO as RX.
    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz     = AM2301_RESOLUTION_HZ;
    tx_cfg.mem_block_symbols = AM2301_MEM_SYMBOLS;
    tx_cfg.gpio_num          = pin;
    tx_cfg.trans_queue_depth = 2;
    tx_cfg.flags.init_level   = 1;   // idle the OD line high (released) before TX
    err = rmt_new_tx_channel(&tx_cfg, &s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tx channel: %s", esp_err_to_name(err));
        return false;
    }

    rmt_copy_encoder_config_t enc_cfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&enc_cfg, &s_copy_enc));

    // Open-drain so a "high" TX symbol releases the line (high-Z) instead of
    // fighting the sensor when it pulls low.  The line then idles high through
    // a pull-up: the breakout's own 5.1 kΩ, plus the internal one as backup.
    gpio_od_enable(pin);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);

    s_started = true;
    xTaskCreate(am2301_task, "am2301", 3072, nullptr, 2, nullptr);
    ESP_LOGI(TAG, "AM2301 reader started on GPIO%d", (int)pin);
    return true;
}

Am2301Data am2301GetData() {
    portENTER_CRITICAL(&s_mux);
    Am2301Data d = s_data;
    portEXIT_CRITICAL(&s_mux);
    return d;
}
