#include "heapdiag.hpp"

#ifdef HEAP_DIAG

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdint.h>

static const char* TAG = "heapdiag";

// Previous-mark free sizes, so each line reports the delta attributable to the
// work done since the last mark. SIZE_MAX sentinel = first call (no delta yet).
static size_t s_prevInt = SIZE_MAX;
static size_t s_prevPsram = SIZE_MAX;

void heapDiagMark(const char* label)
{
    const size_t freeInt    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t minInt     = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t largestInt = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t freePsram  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    if (s_prevInt == SIZE_MAX) {
        // First mark — baseline, no delta.
        ESP_LOGI(TAG, "%-22s INT free=%6u min=%6u largest=%6u | PSRAM free=%9u",
                 label, (unsigned)freeInt, (unsigned)minInt,
                 (unsigned)largestInt, (unsigned)freePsram);
    } else {
        // Signed deltas: negative dINT = this stage consumed internal DRAM.
        const long dInt   = (long)freeInt   - (long)s_prevInt;
        const long dPsram = (long)freePsram - (long)s_prevPsram;
        ESP_LOGI(TAG,
                 "%-22s INT free=%6u min=%6u largest=%6u | PSRAM free=%9u | dINT=%+ld dPSRAM=%+ld",
                 label, (unsigned)freeInt, (unsigned)minInt,
                 (unsigned)largestInt, (unsigned)freePsram, dInt, dPsram);
    }

    s_prevInt   = freeInt;
    s_prevPsram = freePsram;
}

#endif // HEAP_DIAG
