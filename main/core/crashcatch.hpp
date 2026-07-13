#pragma once
// Captures the last panic/abort into RTC memory (survives the panic-triggered
// reboot; cleared only on power-on) so the crash PC + a shallow stack snapshot
// can be read back and served over the web — no coredump partition, no serial
// monitor needed. See crashcatch.cpp for the wrapped panic-handler mechanism.
#include <cstdint>

struct CrashInfo {
    static constexpr int STACK_WORDS = 32;
    bool     valid;
    uint32_t pc;                    // mepc — the faulting instruction
    uint32_t ra;                    // return address (caller)
    uint32_t sp;
    uint32_t mcause;                // RISC-V trap cause
    uint32_t mtval;                 // faulting address / bad value
    int32_t  core;
    char     task[16];              // FreeRTOS task that crashed
    char     reason[32];            // IDF exception string (e.g. "Store access fault")
    uint32_t stack[STACK_WORDS];    // words from sp upward — return addresses live here
};

// Read the RTC-persisted crash record. Returns false if none was captured since
// the last power-on. Safe to call any time after boot.
bool crashCatchGet(CrashInfo& out);

// Log a one-line summary at boot if a crash record is present. Call after NVS.
void crashCatchInit();
