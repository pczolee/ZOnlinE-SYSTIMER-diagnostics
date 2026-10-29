#pragma once
#include <stdint.h>
// 716: same time bound for healthy and recovery reads. S3 SYSTIMER is 16 MHz;
// CCOUNT follows CPU MHz, not half the CPU clock. One tick allows read latency.
#define ZONLINE_SYSTICK_RECOVERY_ATTEMPTS 8U
#define ZONLINE_SYSTICK_RECOVERY_MIN_READS 3U
#define ZONLINE_SYSTICK_RECOVERY_MAX_PENDING 16U
#define ZONLINE_SYSTICK_RECOVERY_MAX_CYCLES 1600000U
static inline unsigned zonlineSystickTimePlausible(uint64_t candidate, uint64_t lastGood,
    uint32_t period, uint32_t ccountElapsed, uint32_t cpuMHz) {
    if (!period || !lastGood || candidate < lastGood || cpuMHz < 80U || cpuMHz > 240U)
        return 0;
    // A healthy ISR runs every millisecond. Beyond one second the modulo
    // CCOUNT reference is no longer accepted as evidence of a healthy interval.
    if (ccountElapsed > cpuMHz * 1000000U) return 0;
    const uint64_t allowance=((uint64_t)ccountElapsed*16U)/cpuMHz + period;
    return candidate-lastGood <= allowance;
}
static inline unsigned zonlineSystickRecoveryPlausible(
    uint64_t candidate, uint64_t lastGood, uint32_t period,
    uint32_t handled, uint32_t ccountElapsed, uint32_t cpuMHz, uint32_t *pending) {
    if (ccountElapsed > ZONLINE_SYSTICK_RECOVERY_MAX_CYCLES ||
        !zonlineSystickTimePlausible(candidate,lastGood,period,ccountElapsed,cpuMHz))
        return 0;
    const uint32_t nowTicks=(uint32_t)(candidate/period);
    const uint32_t modular=nowTicks-handled;
    if(modular<=ZONLINE_SYSTICK_RECOVERY_MAX_PENDING) { *pending=modular; return 1; }
    // Never hide an already advanced RTOS clock by reporting zero pending ticks.
    return 0;
}
typedef struct {
    uint32_t attempts, recovered, rejected, reason1, reason2, reason4, backward;
    uint32_t reason8;
} ZonlineSystickRecoveryStats;
#ifdef __cplusplus
extern "C" {
#endif
void zonlineSystickRecoveryGetStats(unsigned core, ZonlineSystickRecoveryStats *out);
#ifdef __cplusplus
}
#endif
