// Panic capture to RTC memory — see crashcatch.hpp.
//
// The panic entry (panic_handler.c) calls esp_panic_handler(&info) by name, so
// `-Wl,--wrap=esp_panic_handler` (wired in main/CMakeLists.txt) routes it to our
// __wrap_ shim first. The shim copies the crash context into an RTC_NOINIT_ATTR
// record — which survives the panic-triggered restart but is NOT re-zeroed by
// startup — then hands off to __real_esp_panic_handler so the normal panic
// print + reboot proceed unchanged.
//
// Everything in the shim must be allocation-free and lock-free: the system is
// in an undefined state (heap possibly corrupt, scheduler halted). Plain memory
// writes and leaf libc calls only. The stack read is guarded by
// esp_stack_ptr_is_sane() so a corrupt SP can't trigger a fault-in-the-fault.

#include "crashcatch.hpp"

#include "esp_attr.h"           // RTC_NOINIT_ATTR
#include "esp_memory_utils.h"   // esp_stack_ptr_is_sane
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "riscv/rvruntime-frames.h"     // RvExcFrame
#include "esp_private/panic_internal.h" // panic_info_t

#include <cstring>

static const char* TAG = "crash";

static constexpr uint32_t CRASH_MAGIC = 0x43524148u;   // 'CRAH'

struct RtcCrash {
    uint32_t magic;
    uint32_t pc, ra, sp, mcause, mtval;
    int32_t  core;
    char     task[16];
    char     reason[32];
    uint32_t stack[CrashInfo::STACK_WORDS];
};

// Uninitialised at power-on (magic is then garbage → rejected by the check).
static RTC_NOINIT_ATTR RtcCrash s_rtc;

extern "C" void __real_esp_panic_handler(panic_info_t* info);

extern "C" void __wrap_esp_panic_handler(panic_info_t* info)
{
    RtcCrash* c = &s_rtc;
    c->magic = CRASH_MAGIC;
    c->core  = info ? info->core : -1;

    c->reason[0] = '\0';
    if (info && info->reason) {
        strncpy(c->reason, info->reason, sizeof(c->reason) - 1);
        c->reason[sizeof(c->reason) - 1] = '\0';
    }

    const RvExcFrame* f = (info && info->frame) ? (const RvExcFrame*)info->frame : nullptr;
    if (f) {
        c->pc     = (uint32_t)f->mepc;
        c->ra     = (uint32_t)f->ra;
        c->sp     = (uint32_t)f->sp;
        c->mcause = (uint32_t)f->mcause;
        c->mtval  = (uint32_t)f->mtval;
        // Reading forward from a sane SP walks the used stack (where caller
        // return addresses sit). Guard against a corrupt SP so we never fault
        // inside the panic handler.
        if (esp_stack_ptr_is_sane((uint32_t)f->sp)) {
            const uint32_t* sp = (const uint32_t*)(uintptr_t)f->sp;
            for (int i = 0; i < CrashInfo::STACK_WORDS; i++) c->stack[i] = sp[i];
        } else {
            memset(c->stack, 0, sizeof(c->stack));
        }
    } else {
        c->pc = c->ra = c->sp = c->mcause = c->mtval = 0;
        memset(c->stack, 0, sizeof(c->stack));
    }

    c->task[0] = '\0';
    TaskHandle_t th = xTaskGetCurrentTaskHandleForCore(c->core >= 0 ? c->core : 0);
    if (th) {
        const char* n = pcTaskGetName(th);
        if (n) { strncpy(c->task, n, sizeof(c->task) - 1); c->task[sizeof(c->task) - 1] = '\0'; }
    }

    __real_esp_panic_handler(info);
}

bool crashCatchGet(CrashInfo& out)
{
    if (s_rtc.magic != CRASH_MAGIC) return false;
    out.valid  = true;
    out.pc     = s_rtc.pc;
    out.ra     = s_rtc.ra;
    out.sp     = s_rtc.sp;
    out.mcause = s_rtc.mcause;
    out.mtval  = s_rtc.mtval;
    out.core   = s_rtc.core;
    memcpy(out.task,   s_rtc.task,   sizeof(out.task));
    memcpy(out.reason, s_rtc.reason, sizeof(out.reason));
    memcpy(out.stack,  s_rtc.stack,  sizeof(out.stack));
    out.task[sizeof(out.task) - 1]     = '\0';
    out.reason[sizeof(out.reason) - 1] = '\0';
    return true;
}

void crashCatchInit()
{
    CrashInfo ci;
    if (crashCatchGet(ci)) {
        ESP_LOGW(TAG, "last crash: task=%s reason=%s pc=0x%08lx ra=0x%08lx mtval=0x%08lx",
                 ci.task, ci.reason, (unsigned long)ci.pc, (unsigned long)ci.ra,
                 (unsigned long)ci.mtval);
    } else {
        ESP_LOGI(TAG, "no crash on record");
    }
}
