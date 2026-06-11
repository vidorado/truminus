#pragma once
//
// TruMinus — internal-DRAM bring-up probes.
//
// This board (JC4880P443C / ESP32-P4 over esp_hosted) is internal-DRAM-starved:
// PSRAM sits ~26 MB free while internal SRAM runs ~95 % full at steady state
// (see pio-idf-p4 SKILL §16). Per-allocation caller attribution via heap
// tracing is impractical here (RISC-V needs frame pointers, which overflow the
// bootloader). The cheap, build-real alternative is differential measurement:
// snapshot free internal/PSRAM at each subsystem bring-up boundary and read off
// who consumes what from the deltas.
//
// heapDiagMark("label") logs free / minimum-ever / largest-block for
// MALLOC_CAP_INTERNAL plus free PSRAM, and the delta vs the previous mark.
// A negative dINT = that subsystem just consumed internal DRAM. Use this to
// A/B the PSRAM-steering knobs (ESP_HOSTED_MEMPOOL_PREFER_SPIRAM,
// ESP_HOSTED_DFLT_TASK_FROM_SPIRAM, BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL).
//
// Gated by HEAP_DIAG (defined in flags.h). When off, every call compiles away.

#ifdef HEAP_DIAG
void heapDiagMark(const char* label);
#else
static inline void heapDiagMark(const char*) {}
#endif
