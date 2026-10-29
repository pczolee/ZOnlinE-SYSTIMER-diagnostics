#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
// Exactly 256 RTC bytes for both cores. The first unreported event per core survives rapid reboots.
typedef struct { uint32_t magic, checksum, kind, core, rtcLo, rtcHi, ccount, data[25]; } ZonlineFirstFault;
// Independent versioned 96-byte RTC record per CPU core. Fault-only capture.
// v2.6.712.1: 64 bytes reclaimed overall; duplicate context remains in ZFF708.
// Never changes the legacy ZFF708 128-byte/core record or its CRC.
typedef struct {
    uint32_t magic, checksum, version, core;
    uint32_t first_lo, first_hi;
    uint32_t u1_latched_pre_lo, u1_latched_pre_hi;
    uint32_t u1_op_pre;
    uint32_t u0_latched_pre_lo, u0_latched_pre_hi, u0_op_pre;
    uint32_t u0_snapshot_lo, u0_snapshot_hi, u0_polls;
    uint32_t u1_latched_post_lo, u1_latched_post_hi;
    uint32_t confirm_lo, confirm_hi, confirm_meta;
    uint32_t ccount_duration, unit1_load_hi, unit1_load_lo, flags;
} ZonlineSystimerProbe;
// ZSP715: same 96-byte RTC footprint, repurposes these 721 fields:
// u0_latched_pre_lo/hi, u0_op_pre -> fault SYSTIMER/TIMG0/TIMG1 DATE;
// u0_polls -> packed sample count, unstable count, DATE mismatch bits, baseline flag;
// u1_latched_post_lo/hi -> 32-sample HI AND/OR bitmaps;
// unit1_load_hi/lo -> last accepted sample HI/LO.
// The full 32 (LO, HI, CCOUNT) samples reside in fixed DRAM on the fault
// path only, while the bit-pattern summary survives warm reset. No new RTC bytes.
#define ZONLINE_SYSTIMER_PROBE_715_VERSION 0x00020715U
#define ZONLINE_SYSTIMER_PROBE_716_VERSION 0x00020716U
// 716 overlay: u1_latched_pre_lo = matching first-fault checksum;
// u1_op_pre = saturated attempts[15:0]/recovered[31:16];
// flags[31:16] = saturated rejected count, bit 7 = recovery counters present.
// New first-fault kinds: 3=bounded HAL read, 4=SysTick, 5=panic.
// kind 4 data[23]=boot token and data[24]=ELF SHA256 32-bit prefix (not load HI).
// kind 5 retains the complete old panic frame; data[24]=ELF prefix, boot unknown.
void zonlineFaultCaptureIdentity(uint32_t elfPrefix, uint32_t bootToken);
void zonlineSystimerProbeRecovery(unsigned core, uint32_t attempts, uint32_t recovered, uint32_t rejected);
void zonlineTimerReadFault(unsigned core, unsigned unit, unsigned reason,
    uint32_t lo, uint32_t hi, uint32_t polls, uint32_t retries);
#define ZONLINE_SYSTIMER_TRACE_LIMIT 32U
#define ZONLINE_SYSTIMER_TRACE_CPU_CYCLES 100000U
#define ZONLINE_SYSTIMER_TRACE_META_COUNT(m) ((uint8_t)((m)&63U))
#define ZONLINE_SYSTIMER_TRACE_META_UNSTABLE(m) ((uint8_t)(((m)>>6)&63U))
#define ZONLINE_SYSTIMER_TRACE_META_DATE_MISMATCH(m) (((m)>>12)&7U)
#define ZONLINE_SYSTIMER_TRACE_META_BASELINE_VALID(m) (((m)>>15)&1U)
#define ZONLINE_SYSTIMER_TRACE_META_EXPIRED(m) (((m)>>16)&1U)
// Flags: 1=pre latched, 2=complete, 4=UNIT0 snapshot valid,
// 8=UNIT0 VALID timeout, 16=UNIT0 unstable LO, 32=UNIT0 read attempted.
void zonlineSystimerDateBaselineInit(void);
void zonlineSystimerProbeBegin(unsigned core, uint32_t firstLo, uint32_t firstHi);
void zonlineSystimerProbeFinish(unsigned core, uint32_t checkLo, uint32_t checkHi, uint32_t checkMeta);
bool zonlineSystimerProbePrevious(unsigned core, ZonlineSystimerProbe *out);
void zonlineSystimerProbeUsbEmitted(unsigned core, uint32_t checksum);
void zonlineFirstPanic(const void *frame);
void zonlineTimerFault(unsigned core, uint32_t reason, uint32_t period, uint32_t handled,
                       uint32_t pending, uint32_t lo, uint32_t hi, uint32_t opBefore,
                       uint32_t polls, uint32_t retries);
void zonlineFirstFaultBoot(bool preserve);
void zonlineFirstFaultUsbEmitted(unsigned core, uint32_t checksum);
void zonlineTimerFaultDetail(unsigned core, uint32_t lastLo, uint32_t lastHi,
    uint32_t lastCycles, uint32_t lastHandled, uint32_t checkLo, uint32_t checkHi, uint32_t checkMeta);
bool zonlineFirstFaultPrevious(unsigned core, ZonlineFirstFault *out);
#ifdef __cplusplus
}
void runtimeFirstFaultLogPrevious();
void runtimeFirstFaultUsbEarly();
void runtimeFirstFaultUsbTick();
void runtimeFaultContextTick();
#endif
