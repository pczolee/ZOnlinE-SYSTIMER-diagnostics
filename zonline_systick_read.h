#pragma once
#include "zonline_systick_guard_policy.h"
// Include after the SDK LL declarations. Host tests supply the same read API.
static inline __attribute__((always_inline)) unsigned zonlineSystimerReadBounded(
    systimer_dev_t *dev,unsigned unit,uint32_t *lo,uint32_t *hi,uint32_t *polls,uint32_t *retries) {
    *lo=0;*hi=0;*polls=0;*retries=0;
    systimer_ll_counter_snapshot(dev,unit);
    while(!systimer_ll_is_counter_value_valid(dev,unit)) {
        if(++*polls>=ZONLINE_SYSTICK_MAX_POLLS) return 1;
    }
    for(*retries=0;*retries<4;++*retries) {
        *lo=systimer_ll_get_counter_value_low(dev,unit);
        *hi=systimer_ll_get_counter_value_high(dev,unit);
        if(*lo==systimer_ll_get_counter_value_low(dev,unit)) return 0;
    }
    return 2;
}
static inline __attribute__((always_inline)) unsigned zonlineSystickReadBounded(
    systimer_dev_t *dev,uint32_t *lo,uint32_t *hi,uint32_t *polls,uint32_t *retries) {
    return zonlineSystimerReadBounded(dev,1,lo,hi,polls,retries);
}
