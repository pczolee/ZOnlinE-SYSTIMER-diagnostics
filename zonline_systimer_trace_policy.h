#pragma once
#include <stdint.h>
#include <stdbool.h>
// ISR-safe fault-only aggregator; no timers, allocations or drivers involved.
typedef struct {
  uint32_t hi_and, hi_or, last_lo, last_hi;
  uint32_t valid, unstable;
} ZonlineSystimerTraceSummary;
static inline __attribute__((always_inline)) void zonlineSystimerTraceReset(ZonlineSystimerTraceSummary *s) {
  s->hi_and=UINT32_MAX;s->hi_or=0;
  s->last_lo=0;s->last_hi=0;s->valid=0;s->unstable=0;
}
static inline __attribute__((always_inline)) void zonlineSystimerTraceAccept(ZonlineSystimerTraceSummary *s,uint32_t lo,uint32_t hi){
  s->hi_and &= hi;s->hi_or |= hi;s->last_lo=lo;s->last_hi=hi;
  if(s->valid<63U) ++s->valid;
}
static inline __attribute__((always_inline)) void zonlineSystimerTraceReject(ZonlineSystimerTraceSummary *s){
  if(s->unstable<63U) ++s->unstable;
}
static inline __attribute__((always_inline)) uint32_t zonlineSystimerTracePackMeta(const ZonlineSystimerTraceSummary *s,
                                                   uint32_t mismatch,bool baseline,bool expired){
  return (s->valid&63U)|((s->unstable&63U)<<6)|((mismatch&7U)<<12)|
         (baseline?(1U<<15):0U)|(expired?(1U<<16):0U);
}
