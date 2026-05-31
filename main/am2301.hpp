#pragma once
#include <stdint.h>
#include "driver/gpio.h"

// AM2301 / DHT22 external (outdoor) temperature + humidity sensor.
//
// Single-wire bidirectional protocol read with the RMT peripheral: a TX
// channel drives the >=1 ms start-low pulse and an RX channel captures the
// 40-bit response, both bound to the same GPIO in open-drain loopback mode.
// This keeps the timing-critical bit decode off the CPU — no `portENTER_-
// CRITICAL` / interrupts-disabled window (a 5 ms one would stutter WiFi /
// LVGL on this mains-powered controller).
//
// See `.claude/skills/...` (TODO) for the byte layout; checksum is
// `b4 == (b0 + b1 + b2 + b3) & 0xFF`, humidity = (b0<<8|b1)/10, temp =
// sign(b2&0x80) * ((b2&0x7F)<<8 | b3) / 10.

struct Am2301Data {
    float    tempC;       // last valid temperature [°C]; NAN until first read
    float    humidity;    // last valid relative humidity [%]; NAN until first read
    bool     valid;       // true after at least one good reading
    uint32_t lastMs;      // esp_timer_get_time()/1000 at last valid reception
};

// Configure the RMT channels and spawn the 30 s polling task.  Idempotent:
// a second call is a no-op.  `pin` is the DATA line (GPIO52 on JC4880-P4).
// Returns false if the RMT channels could not be created.
bool am2301Start(gpio_num_t pin);

// Thread-safe snapshot accessor (main loop + WS pump).
Am2301Data am2301GetData();
