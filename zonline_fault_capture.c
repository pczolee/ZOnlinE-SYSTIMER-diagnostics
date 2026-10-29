// ZOnlinE 708: bounded, allocation/lock/flash-free first fault retention.
#include "zonline_fault_capture.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "soc/systimer_struct.h"
#include "soc/rtc_cntl_struct.h"
#include "soc/soc.h"
#include "zonline_systimer_trace_policy.h"
#include "xtensa_context.h"
#include <stddef.h>
#define FAULT_MAGIC 0x5A460708U
#define FAULT_EMITTED 0x5A46E708U
_Static_assert(sizeof(ZonlineFirstFault)==128, "RTC footprint changed");
_Static_assert(offsetof(XtExcFrame,pc)==4, "Review exception frame ABI");
_Static_assert(offsetof(XtExcFrame,lcount)==96, "Review exception frame ABI");
_Static_assert(offsetof(systimer_dev_t,unit_load_val[1].hi)==0x14, "Review S3 UNIT1 load-value register layout");
_Static_assert(offsetof(systimer_dev_t,unit_load_val[1].lo)==0x18, "Review S3 UNIT1 load-value register layout");
// A separate fault-only journal avoids changing the established ZFF708 wire
// protocol and its RTC retention. Never access this from the healthy tick path.
#define PROBE_MAGIC 0x5A535032U /* ZSP2, compact v2.6.712.1 */
#define PROBE_EMITTED 0x5A535046U
#define PROBE_VERSION ZONLINE_SYSTIMER_PROBE_716_VERSION
#define LEGACY_PROBE_VERSION 0x00020721U
_Static_assert(sizeof(ZonlineSystimerProbe)==96, "SYSTIMER probe RTC footprint changed");
RTC_NOINIT_ATTR static volatile ZonlineSystimerProbe retainedProbe[2];
static ZonlineSystimerProbe previousProbe[2];
DRAM_ATTR static volatile uint8_t probeActive[2];
DRAM_ATTR static volatile struct { uint32_t lo, hi, ccount; } faultSample[2][ZONLINE_SYSTIMER_TRACE_LIMIT];
DRAM_ATTR static uint32_t dateBaseline[3];
DRAM_ATTR static volatile uint8_t dateBaselineValid;
DRAM_ATTR static uint32_t captureElfPrefix, captureBootToken;
void zonlineFaultCaptureIdentity(uint32_t elfPrefix,uint32_t bootToken) {
    captureElfPrefix=elfPrefix;captureBootToken=bootToken;
}
// ESP32-S3 TRM: SYSTIMER DATE +0xFC, TIMGx NTIMERS DATE +0xF8.
// The DATE registers are RW: compare with values sampled after boot, do not
// treat their factory reset values as immutable read-only constants.
#ifndef ZONLINE_SYSTIMER_DATE
#define ZONLINE_SYSTIMER_DATE (*(volatile const uint32_t *)(uintptr_t)(DR_REG_SYSTIMER_BASE + 0xFCU))
#endif
#ifndef ZONLINE_TIMG0_DATE
#define ZONLINE_TIMG0_DATE (*(volatile const uint32_t *)(uintptr_t)(DR_REG_TIMERGROUP0_BASE + 0xF8U))
#endif
#ifndef ZONLINE_TIMG1_DATE
#define ZONLINE_TIMG1_DATE (*(volatile const uint32_t *)(uintptr_t)(DR_REG_TIMERGROUP1_BASE + 0xF8U))
#endif
void IRAM_ATTR zonlineSystimerDateBaselineInit(void) {
    dateBaselineValid=0;
    dateBaseline[0]=ZONLINE_SYSTIMER_DATE;
    dateBaseline[1]=ZONLINE_TIMG0_DATE;
    dateBaseline[2]=ZONLINE_TIMG1_DATE;
    __atomic_store_n(&dateBaselineValid,1U,__ATOMIC_RELEASE);
}

static inline __attribute__((always_inline)) uint32_t probeChecksum(const volatile ZonlineSystimerProbe *p) {
    const volatile uint32_t *words=(const volatile uint32_t *)p;
    uint32_t h=2166136261U;
    for(unsigned i=2;i<24;i++) h=(h^words[i])*16777619U;
    return h;
}
static inline __attribute__((always_inline)) bool validProbe(const volatile ZonlineSystimerProbe *p) {
    return (p->magic==PROBE_MAGIC || p->magic==PROBE_EMITTED) &&
        (p->version==PROBE_VERSION || p->version==ZONLINE_SYSTIMER_PROBE_715_VERSION ||
         p->version==LEGACY_PROBE_VERSION) && p->checksum==probeChecksum(p) && (p->flags & 3U)==3U;
}
RTC_NOINIT_ATTR static volatile ZonlineFirstFault retained[2];
static ZonlineFirstFault previous[2];
DRAM_ATTR static volatile uint32_t captured[2];
static inline __attribute__((always_inline)) uint32_t checksum(const volatile ZonlineFirstFault *r) {
    const volatile uint32_t *p=&r->kind; uint32_t h=2166136261U;
    for(unsigned i=0;i<30;i++) h=(h^p[i])*16777619U;
    return h;
}
// Extension writes never destroy the committed base (data[0..15]). If a
// reset interrupts data[16..24], its original base checksum still validates.
static inline __attribute__((always_inline)) uint32_t baseChecksum(const volatile ZonlineFirstFault *r) {
    const volatile uint32_t *p=&r->kind; uint32_t h=2166136261U;
    for(unsigned i=0;i<30;i++) h=(h^(i<21?p[i]:0U))*16777619U;
    return h;
}
static inline __attribute__((always_inline)) bool validRecord(const volatile ZonlineFirstFault *r) {
    return r->checksum==checksum(r) || ((r->kind==2 || r->kind==4) && r->checksum==baseChecksum(r));
}
static inline __attribute__((always_inline)) volatile ZonlineFirstFault *begin(unsigned core, unsigned kind) {
    if(core>1 || captured[core]) return 0;
    // Preserve a committed, not-yet-emitted capsule even if setup never ran
    // on the intervening boot. No RTC allocation and no flash write here.
    if(retained[core].magic==FAULT_MAGIC && validRecord(&retained[core])) return 0;
    captured[core]=1; // same-core nested faults must not replace this frame
    volatile ZonlineFirstFault *r=&retained[core];
    r->magic=0; r->kind=kind; r->core=core; r->ccount=esp_cpu_get_cycle_count();
    r->rtcLo=0; r->rtcHi=0;
    RTCCNTL.time_update.update=1;
    for(unsigned i=0;i<4;i++) {
        uint32_t lo=RTCCNTL.time_low0, hi=RTCCNTL.time_high0.rtc_timer_value0_high;
        if(lo==RTCCNTL.time_low0) {r->rtcLo=lo;r->rtcHi=hi;break;}
    }
    for(unsigned i=0;i<25;i++) r->data[i]=0;
    return r;
}
static inline __attribute__((always_inline)) void commit(volatile ZonlineFirstFault *r) {
    r->checksum=checksum(r);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    r->magic=FAULT_MAGIC; // commit last; a truncated capture is never accepted
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
void IRAM_ATTR zonlineFirstPanic(const void *frame) {
    volatile ZonlineFirstFault *r=begin(esp_cpu_get_core_id(),5);
    if(!r) return;
    const uint32_t *p=(const uint32_t *)frame;
    for(unsigned i=0;i<24;i++) r->data[i]=p[i+1]; // PC .. LCOUNT
    r->data[24]=captureElfPrefix;
    commit(r);
}
void IRAM_ATTR zonlineTimerFault(unsigned core, uint32_t reason, uint32_t period, uint32_t handled,
                       uint32_t pending, uint32_t lo, uint32_t hi, uint32_t opBefore,
                       uint32_t polls, uint32_t retries) {
    volatile ZonlineFirstFault *r=begin(core,4);
    if(!r) return;
    r->data[0]=reason;r->data[1]=period;r->data[2]=handled;r->data[3]=pending;
    r->data[4]=lo;r->data[5]=hi;r->data[6]=opBefore;r->data[7]=SYSTIMER.unit_op[1].val;
    r->data[8]=polls;r->data[9]=retries;r->data[10]=SYSTIMER.conf.val;
    r->data[11]=SYSTIMER.int_raw.val;r->data[12]=SYSTIMER.int_st.val;r->data[13]=SYSTIMER.int_ena.val;
    r->data[14]=SYSTIMER.target_conf[0].val;r->data[15]=SYSTIMER.target_conf[1].val;
    commit(r);
}
void IRAM_ATTR zonlineTimerReadFault(unsigned core,unsigned unit,unsigned reason,
    uint32_t lo,uint32_t hi,uint32_t polls,uint32_t retries) {
    volatile ZonlineFirstFault *r=begin(core,3);
    if(!r) return;
    r->data[0]=reason;r->data[1]=unit;r->data[2]=lo;r->data[3]=hi;
    r->data[4]=polls;r->data[5]=retries;
    r->data[6]=unit<2U?SYSTIMER.unit_op[unit].val:0;
    r->data[7]=SYSTIMER.conf.val;r->data[8]=SYSTIMER.int_raw.val;
    r->data[9]=SYSTIMER.int_st.val;r->data[10]=SYSTIMER.int_ena.val;
    r->data[23]=captureBootToken;r->data[24]=captureElfPrefix;
    commit(r);
}
void IRAM_ATTR zonlineTimerFaultDetail(unsigned core, uint32_t lastLo, uint32_t lastHi,
    uint32_t lastCycles, uint32_t lastHandled, uint32_t checkLo, uint32_t checkHi, uint32_t checkMeta) {
    if(core>1 || !captured[core]) return;
    volatile ZonlineFirstFault *r=&retained[core];
    if(r->magic!=FAULT_MAGIC || r->kind!=4) return;
    // Keep the base commit valid throughout an interrupted extension write.
    r->data[16]=lastLo; r->data[17]=lastHi; r->data[18]=lastCycles; r->data[19]=lastHandled;
    r->data[20]=checkLo; r->data[21]=checkHi; r->data[22]=checkMeta;
    // 716 identity replaces the two load-value words; no extra RTC allocation.
    r->data[23]=captureBootToken;r->data[24]=captureElfPrefix;
    commit(r);
}
// This function is ONLY called after a SysTick guard fault and after the
// original emergency reset has been armed. It does not sample UNIT0 yet:
// capture the pre-existing read-only value registers before the existing
// confirm-read writes UNIT1.UPDATE again.
void IRAM_ATTR zonlineSystimerProbeBegin(unsigned core, uint32_t firstLo, uint32_t firstHi) {
    if(core>1 || !captured[core] || probeActive[core]) return;
    volatile ZonlineFirstFault *base=&retained[core];
    if(base->magic!=FAULT_MAGIC || base->kind!=4) return;
    // Preserve an earlier probe until the USB transport has emitted it.
    if(retainedProbe[core].magic==PROBE_MAGIC && validProbe(&retainedProbe[core])) return;
    volatile ZonlineSystimerProbe *p=&retainedProbe[core];
    p->magic=0;
    volatile uint32_t *w=(volatile uint32_t *)p;
    for(unsigned i=1;i<24;i++) w[i]=0;
    p->version=PROBE_VERSION; p->core=core;
    p->ccount_duration=esp_cpu_get_cycle_count();
    p->first_lo=firstLo; p->first_hi=firstHi;
    p->u1_latched_pre_lo=SYSTIMER.unit_val[1].lo.val;
    p->u1_latched_pre_hi=SYSTIMER.unit_val[1].hi.val;
    p->u1_op_pre=SYSTIMER.unit_op[1].val;
    // 715 overlay: preserve fault-time DATE values instead of stale UNIT0 latch.
    p->u0_latched_pre_lo=ZONLINE_SYSTIMER_DATE;
    p->u0_latched_pre_hi=ZONLINE_TIMG0_DATE;
    p->u0_op_pre=ZONLINE_TIMG1_DATE;
    p->flags=1U;
    probeActive[core]=1;
}
void IRAM_ATTR zonlineSystimerProbeRecovery(unsigned core,uint32_t attempts,
    uint32_t recovered,uint32_t rejected) {
    if(core>1 || !probeActive[core]) return;
    retainedProbe[core].u1_op_pre=(attempts>65535U?65535U:attempts)|
        ((recovered>65535U?65535U:recovered)<<16);
    retainedProbe[core].flags|=128U|((rejected>65535U?65535U:rejected)<<16);
}

void IRAM_ATTR zonlineSystimerProbeFinish(unsigned core, uint32_t checkLo, uint32_t checkHi, uint32_t checkMeta) {
    if(core>1 || !probeActive[core]) return;
    volatile ZonlineSystimerProbe *p=&retainedProbe[core];
    if(p->magic!=0 || p->version!=PROBE_VERSION || p->core!=core) { probeActive[core]=0; return; }
    p->confirm_lo=checkLo; p->confirm_hi=checkHi; p->confirm_meta=checkMeta;
    // No UNIT1 load register in 715 (already preserved in ZFF708).
    // The reused words hold HI AND/OR and the last accepted sample.
    // Last-resort UNIT0 reference. No heap, locks, logs or unbounded waits.
    // The UNIT0 snapshot is shared with esp_timer and may be overwritten by
    // another CPU; an unstable LO is flagged rather than being accepted.
    p->flags|=32U;
    SYSTIMER.unit_op[0].timer_unit_update=1;
    bool ready=false;
    for(uint32_t i=0;i<1024U;i++) {
        if(SYSTIMER.unit_op[0].timer_unit_value_valid) {p->u0_polls=i;ready=true;break;}
        p->u0_polls=i+1U;
    }
    if(!ready) {p->flags|=8U;}
    else {
        bool stable=false;
        for(uint32_t i=0;i<4U;i++) {
            uint32_t lo=SYSTIMER.unit_val[0].lo.val;
            uint32_t hi=SYSTIMER.unit_val[0].hi.val;
            uint32_t lo2=SYSTIMER.unit_val[0].lo.val;
            p->u0_snapshot_lo=lo;p->u0_snapshot_hi=hi;
            if(lo==lo2) {stable=true;break;}
        }
        if(stable) {p->flags|=4U;}
        else {p->flags|=16U;}
    }
    // 32 rapid, full UPDATE/VALID/LO-HI-LO cycles, strictly fault-only.
    // Samples are recorded in DRAM with individual CCOUNT timestamps; the
    // 96-byte RTC capsule retains count, bitwise HI AND/OR and final sample.
    // Never perform serial/log/heap/flash operations on this interrupt path.
    const uint32_t scanStart=esp_cpu_get_cycle_count();
    ZonlineSystimerTraceSummary summary;
    zonlineSystimerTraceReset(&summary);
    bool expired=false;
    for(uint32_t i=0;i<ZONLINE_SYSTIMER_TRACE_LIMIT;i++) {
        if((uint32_t)(esp_cpu_get_cycle_count()-scanStart)>ZONLINE_SYSTIMER_TRACE_CPU_CYCLES) {
            expired=true;break;
        }
        SYSTIMER.unit_op[1].timer_unit_update=1;
        bool valid=false;
        for(uint32_t j=0;j<128U;j++) {
            if(SYSTIMER.unit_op[1].timer_unit_value_valid) {valid=true;break;}
        }
        if(!valid) {zonlineSystimerTraceReject(&summary);continue;}
        const uint32_t lo=SYSTIMER.unit_val[1].lo.val;
        const uint32_t hi=SYSTIMER.unit_val[1].hi.val;
        const uint32_t cycles=esp_cpu_get_cycle_count();
        const uint32_t verifyLo=SYSTIMER.unit_val[1].lo.val;
        if(verifyLo!=lo) {zonlineSystimerTraceReject(&summary);continue;}
        faultSample[core][i].lo=lo;
        faultSample[core][i].hi=hi;
        faultSample[core][i].ccount=cycles;
        zonlineSystimerTraceAccept(&summary,lo,hi);
    }
    const uint32_t scanCycles=esp_cpu_get_cycle_count()-scanStart;
    p->u1_latched_post_lo=summary.hi_and;
    p->u1_latched_post_hi=summary.hi_or;
    p->unit1_load_lo=summary.last_lo;
    p->unit1_load_hi=summary.last_hi;
    const bool baselineReady=__atomic_load_n(&dateBaselineValid,__ATOMIC_ACQUIRE)!=0;
    uint32_t mismatch=0;
    if(baselineReady) {
        if(p->u0_latched_pre_lo!=dateBaseline[0]) mismatch|=1U;
        if(p->u0_latched_pre_hi!=dateBaseline[1]) mismatch|=2U;
        if(p->u0_op_pre!=dateBaseline[2]) mismatch|=4U;
    }
    p->u0_polls=zonlineSystimerTracePackMeta(&summary,mismatch,baselineReady,expired);
    (void)scanCycles; // full probe elapsed CCOUNT remains in ccount_duration.
    p->flags|=64U;
    p->ccount_duration=esp_cpu_get_cycle_count()-p->ccount_duration;
    p->flags|=2U;
    p->u1_latched_pre_lo=retained[core].checksum; // exact record association
    p->checksum=probeChecksum(p);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    p->magic=PROBE_MAGIC;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    probeActive[core]=0;
}

bool zonlineSystimerProbePrevious(unsigned core, ZonlineSystimerProbe *out) {
    if(core>1 || (previousProbe[core].magic!=PROBE_MAGIC && previousProbe[core].magic!=PROBE_EMITTED)) return false;
    *out=previousProbe[core];return true;
}
void zonlineSystimerProbeUsbEmitted(unsigned core, uint32_t sum) {
    if(core>1) return;
    if(retainedProbe[core].magic==PROBE_MAGIC && retainedProbe[core].checksum==sum && validProbe(&retainedProbe[core]))
        retainedProbe[core].magic=PROBE_EMITTED;
}
void zonlineFirstFaultUsbEmitted(unsigned core, uint32_t sum) {
    if(core>1) return;
    // Never acknowledge a different/newer record using an older DRAM copy.
    if(retained[core].magic==FAULT_MAGIC && retained[core].checksum==sum && checksum(&retained[core])==sum)
        retained[core].magic=FAULT_EMITTED;
}
void zonlineFirstFaultBoot(bool preserve) {
    for(unsigned c=0;c<2;c++) {
        previous[c].magic=0;
        if(preserve && (retained[c].magic==FAULT_MAGIC || retained[c].magic==FAULT_EMITTED) && validRecord(&retained[c])) {
            const volatile uint32_t *s=(const volatile uint32_t *)&retained[c];
            uint32_t *d=(uint32_t *)&previous[c];
            for(unsigned i=0;i<32;i++) d[i]=s[i];
            if(previous[c].checksum!=checksum(&previous[c])) {
                for(unsigned i=16;i<25;i++) {
                    previous[c].data[i]=0;
                    retained[c].data[i]=0;
                }
            }
        }
        if(!preserve) retained[c].magic=0;
        // A separate 711 capsule has independent CRC/ack and survives the
        // same warm resets as the established first-fault record.
        previousProbe[c].magic=0;
        if(preserve && validProbe(&retainedProbe[c])) {
            const volatile uint32_t *src=(const volatile uint32_t *)&retainedProbe[c];
            uint32_t *dst=(uint32_t *)&previousProbe[c];
            for(unsigned i=0;i<24;i++) dst[i]=src[i];
        }
        if(!preserve) retainedProbe[c].magic=0;
        probeActive[c]=0;
    }
}
bool zonlineFirstFaultPrevious(unsigned core, ZonlineFirstFault *out) {
    if(core>1 || (previous[core].magic!=FAULT_MAGIC && previous[core].magic!=FAULT_EMITTED)) return false;
    *out=previous[core]; return true;
}
