#pragma once

// FreeRTOS task: polls LCD control state and drains the WS broadcast queue
// every 100 ms so touch inputs reach browsers in ≤100 ms.
void wsPumpTask(void* arg);
