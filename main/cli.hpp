#pragma once

// Starts an esp_console REPL on USB-Serial-JTAG (the P4's /dev/ttyACM0
// port — UART0 is on header pins and unused on this dev setup).  Used while the LCD
// settings screen is unavailable to provision Victron / Ultimatron BLE
// credentials from the serial monitor.  Call after BLE init so reload
// hooks find the subsystem up.
void cliStart();
