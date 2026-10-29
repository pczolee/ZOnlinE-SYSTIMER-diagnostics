#pragma once
#include <stdint.h>
// Bounds are diagnostic containment, not a proof of counter corruption.
// Normal delayed ticks retain upstream modulo-32 accounting, including wrap.
#define ZONLINE_SYSTICK_MAX_BATCH 2048U
#define ZONLINE_SYSTICK_MAX_ROUNDS 32U
#define ZONLINE_SYSTICK_MAX_CYCLES 1600000U /* <=20 ms even at 80 MHz */
#define ZONLINE_SYSTICK_MAX_POLLS 4096U
static inline __attribute__((always_inline)) uint32_t zonlineSystickPending(uint64_t counter, uint32_t period, uint32_t handled) {
    return period ? (uint32_t)(counter / period) - handled : UINT32_MAX;
}
static inline __attribute__((always_inline)) unsigned zonlineSystickReject(uint32_t period, uint32_t expected, uint32_t pending, unsigned initialized) {
    if(period!=expected || !period) return 3U;
    if(initialized && pending>ZONLINE_SYSTICK_MAX_BATCH) return 4U;
    return 0U;
}
