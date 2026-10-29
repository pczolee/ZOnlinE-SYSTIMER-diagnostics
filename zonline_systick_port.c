/*
 * SPDX-FileCopyrightText: 2017-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include "esp_attr.h"
#include "zonline_diagnostic_sdk_guard.h"
#include "zonline_fault_capture.h"
#include "zonline_systick_guard_policy.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
// C bridge: RTC emergency implementation stays in the existing C++ owner.
extern void zonlineSystickEmergency(unsigned core);
#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if ( !CONFIG_FREERTOS_SMP && ( configNUM_CORES > 1 ) )
/* Required for xTaskIncrementTickOtherCores() */
#include "esp_private/freertos_idf_additions_priv.h"
#endif /* ( !CONFIG_FREERTOS_SMP && ( configNUM_CORES > 1 ) ) */

#if CONFIG_FREERTOS_SYSTICK_USES_CCOUNT
#if CONFIG_FREERTOS_CORETIMER_0
#define SYSTICK_INTR_ID     (ETS_INTERNAL_TIMER0_INTR_SOURCE + ETS_INTERNAL_INTR_SOURCE_OFF)
#else /* CONFIG_FREERTOS_CORETIMER_1 */
#define SYSTICK_INTR_ID     (ETS_INTERNAL_TIMER1_INTR_SOURCE + ETS_INTERNAL_INTR_SOURCE_OFF)
#endif
#else /* CONFIG_FREERTOS_SYSTICK_USES_SYSTIMER */
#define SYSTICK_INTR_ID     (ETS_SYSTIMER_TARGET0_INTR_SOURCE)
#endif /* CONFIG_FREERTOS_SYSTICK_USES_CCOUNT */

BaseType_t xPortSysTickHandler(void);

/* --------------------------------------------- SYSTIMER Implementation -----------------------------------------------
 * Implementation of a tick interrupt using the SYSTIMER perpiheral
 * Todo: Abstract setup and operation in a separate SYSTIMER driver (IDF-6976)
 * ------------------------------------------------------------------------------------------------------------------ */

#if CONFIG_FREERTOS_SYSTICK_USES_SYSTIMER

#include "soc/periph_defs.h"
#include "hal/systimer_hal.h"
#include "hal/systimer_ll.h"
#include "zonline_systick_read.h"
#include "zonline_systick_recovery_policy.h"
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_private/systimer.h"
#include "esp_private/periph_ctrl.h"
#ifdef CONFIG_PM_TRACE
#include "esp_private/pm_trace.h"
#endif //CONFIG_PM_TRACE

_Static_assert(SOC_CPU_CORES_NUM <= SOC_SYSTIMER_ALARM_NUM - 1, "the number of cores must match the number of core alarms in SYSTIMER");

void SysTickIsrHandler(void *arg);

static DRAM_ATTR volatile uint32_t s_handled_systicks[configNUM_CORES] = { 0 };
static DRAM_ATTR volatile uint32_t s_handled_inverse[configNUM_CORES];
static DRAM_ATTR uint8_t s_initialized[configNUM_CORES] = { 0 };
// Single writer per core. No locks, heap, or extra normal-path MMIO snapshot.
static DRAM_ATTR struct { uint32_t lo, hi, cycles, handled, cpuMHz; } s_last_good[configNUM_CORES];
static DRAM_ATTR ZonlineSystickRecoveryStats s_recovery[configNUM_CORES];
void IRAM_ATTR zonlineSystickRecoveryGetStats(unsigned core, ZonlineSystickRecoveryStats *out) {
    if(!out || core>=configNUM_CORES) return;
    const volatile uint32_t *src=(const volatile uint32_t *)&s_recovery[core];
    uint32_t *dst=(uint32_t *)out;
    for(unsigned i=0;i<sizeof(*out)/sizeof(uint32_t);i++) dst[i]=src[i];
}
// Fault-only: fresh snapshots with two consecutive plausible results, no locks.
static inline __attribute__((always_inline)) unsigned IRAM_ATTR zonlineSystickTryRecover(
    systimer_dev_t *dev,unsigned core,uint32_t period,uint32_t handled,uint32_t started,
    uint32_t *lo,uint32_t *hi,uint32_t *pending,uint32_t *polls,uint32_t *retries,
    uint32_t *sampleCycles,uint32_t *sampleMHz) {
    if(!s_initialized[core] || !period || !s_last_good[core].cycles) return 0;
    const uint64_t last=((uint64_t)s_last_good[core].hi<<32)|s_last_good[core].lo;
    if(!last) return 0;
    unsigned consecutive=0;
    uint64_t prev=0;
    for(unsigned n=0;n<ZONLINE_SYSTICK_RECOVERY_ATTEMPTS;n++) {
        if((uint32_t)(esp_cpu_get_cycle_count()-started)>ZONLINE_SYSTICK_MAX_CYCLES/4U) return 0;
        uint32_t l=0,h=0,p=0,r=0,candidatePending=0;
        const unsigned rs=zonlineSystickReadBounded(dev,&l,&h,&p,&r);
        const uint32_t cycles=esp_cpu_get_cycle_count();
        const uint32_t mhz=esp_rom_get_cpu_ticks_per_us();
        const uint32_t minMHz=mhz<s_last_good[core].cpuMHz?mhz:s_last_good[core].cpuMHz;
        *polls+=p;*retries+=r;
        if((uint32_t)(esp_cpu_get_cycle_count()-started)>ZONLINE_SYSTICK_MAX_CYCLES/4U) return 0;
        const uint64_t counter=((uint64_t)h<<32)|l;
        const unsigned valid=!rs ? zonlineSystickRecoveryPlausible(counter,last,period,handled,
            cycles-s_last_good[core].cycles,minMHz,&candidatePending) : 0;
        if(valid && (!consecutive || counter>=prev)) {
            consecutive++;
            prev=counter;
            if(n+1>=ZONLINE_SYSTICK_RECOVERY_MIN_READS && consecutive>=2) {
                *lo=l;*hi=h;*pending=candidatePending;
                *sampleCycles=cycles;*sampleMHz=mhz;
                return 1;
            }
        } else consecutive=0;
    }
    return 0;
}


/**
 * @brief Set up the systimer peripheral to generate the tick interrupt
 *
 * Both timer alarms are configured in periodic mode.
 * It is done at the same time so SysTicks for both CPUs occur at the same time or very close.
 * Shifts a time of triggering interrupts for core 0 and core 1.
 */
void vSystimerSetup(void)
{
    unsigned cpuid = xPortGetCoreID();
#ifdef CONFIG_FREERTOS_CORETIMER_SYSTIMER_LVL3
    const unsigned level = ESP_INTR_FLAG_LEVEL3;
#else
    const unsigned level = ESP_INTR_FLAG_LEVEL1;
#endif
    /* Systimer HAL layer object */
    static systimer_hal_context_t systimer_hal;
    /* set system timer interrupt vector */

    ESP_ERROR_CHECK(esp_intr_alloc(ETS_SYSTIMER_TARGET0_INTR_SOURCE + cpuid, ESP_INTR_FLAG_IRAM | level, SysTickIsrHandler, &systimer_hal, NULL));

    if (cpuid == 0) {
        PERIPH_RCC_ACQUIRE_ATOMIC(PERIPH_SYSTIMER_MODULE, ref_count) {
            if (ref_count == 0) {
                systimer_ll_enable_bus_clock(true);
                systimer_ll_reset_register();
            }
        }
        systimer_hal_init(&systimer_hal);
        zonlineSystimerDateBaselineInit();
        systimer_hal_tick_rate_ops_t ops = {
            .ticks_to_us = systimer_ticks_to_us,
            .us_to_ticks = systimer_us_to_ticks,
        };
        systimer_hal_set_tick_rate_ops(&systimer_hal, &ops);
        systimer_ll_set_counter_value(systimer_hal.dev, SYSTIMER_COUNTER_OS_TICK, 0);
        systimer_ll_apply_counter_value(systimer_hal.dev, SYSTIMER_COUNTER_OS_TICK);

        for (cpuid = 0; cpuid < SOC_CPU_CORES_NUM; cpuid++) {
            // Set stall option and alarm mode to default state. Below they will be set to a required state.
            systimer_hal_counter_can_stall_by_cpu(&systimer_hal, SYSTIMER_COUNTER_OS_TICK, cpuid, false);
            uint32_t alarm_id = SYSTIMER_ALARM_OS_TICK_CORE0 + cpuid;
            systimer_hal_select_alarm_mode(&systimer_hal, alarm_id, SYSTIMER_ALARM_MODE_ONESHOT);
        }

        for (cpuid = 0; cpuid < configNUM_CORES; ++cpuid) {
            uint32_t alarm_id = SYSTIMER_ALARM_OS_TICK_CORE0 + cpuid;

            /* configure the timer */
            systimer_hal_connect_alarm_counter(&systimer_hal, alarm_id, SYSTIMER_COUNTER_OS_TICK);
            systimer_hal_set_alarm_period(&systimer_hal, alarm_id, 1000000UL / CONFIG_FREERTOS_HZ);
            systimer_hal_select_alarm_mode(&systimer_hal, alarm_id, SYSTIMER_ALARM_MODE_PERIOD);
            systimer_hal_counter_can_stall_by_cpu(&systimer_hal, SYSTIMER_COUNTER_OS_TICK, cpuid, true);
            if (cpuid == 0) {
                systimer_hal_enable_alarm_int(&systimer_hal, alarm_id);
                systimer_hal_enable_counter(&systimer_hal, SYSTIMER_COUNTER_OS_TICK);
#ifndef CONFIG_FREERTOS_UNICORE
                // SysTick of core 0 and core 1 are shifted by half of period
                systimer_hal_counter_value_advance(&systimer_hal, SYSTIMER_COUNTER_OS_TICK, 1000000UL / CONFIG_FREERTOS_HZ / 2);
#endif
            }
        }
    } else {
        uint32_t alarm_id = SYSTIMER_ALARM_OS_TICK_CORE0 + cpuid;
        systimer_hal_enable_alarm_int(&systimer_hal, alarm_id);
    }
}

/**
 * @brief Systimer interrupt handler.
 *
 * The Systimer interrupt for SysTick works in periodic mode no need to calc the next alarm.
 * If a timer interrupt is ever serviced more than one tick late, it is necessary to process multiple ticks.
 */
// Upstream setup/tick hooks are unchanged. Only UNIT1 ISR read/accounting is bounded.
// No shared snapshot lock, no UNIT0 access, no normal-task counter latching.
void IRAM_ATTR SysTickIsrHandler(void *arg)
{
    const unsigned core=xPortGetCoreID();
    systimer_hal_context_t *hal=(systimer_hal_context_t *)arg;
    const unsigned alarm=SYSTIMER_ALARM_OS_TICK_CORE0+core;
    const uint32_t started=esp_cpu_get_cycle_count();
    unsigned rounds=0, reason=0;
    uint32_t lo=0,hi=0,period=0,pending=0,opBefore=0,polls=0,retries=0,handledBefore=0;
    uint32_t sampleCycles=0,sampleMHz=0;
#ifdef CONFIG_PM_TRACE
    ESP_PM_TRACE_ENTER(TICK, core);
#endif
    do {
        handledBefore=s_handled_systicks[core];
        if(s_initialized[core] && (handledBefore^s_handled_inverse[core])!=UINT32_MAX) {
            reason=7; goto fault; // DRAM accounting mismatch, not attributed to hardware
        }
        systimer_ll_clear_alarm_int(hal->dev,alarm);
        opBefore=hal->dev->unit_op[1].val;
        reason=zonlineSystickReadBounded(hal->dev,&lo,&hi,&polls,&retries);
        sampleCycles=esp_cpu_get_cycle_count();
        sampleMHz=esp_rom_get_cpu_ticks_per_us();
        period=systimer_ll_get_alarm_period(hal->dev,alarm);
        if(!reason) {
            pending=zonlineSystickPending(((uint64_t)hi<<32)|lo,period,s_handled_systicks[core]);
            reason=zonlineSystickReject(period,16000000U/CONFIG_FREERTOS_HZ,pending,s_initialized[core]);
            if(!reason && s_initialized[core] && s_last_good[core].cycles) {
                const uint32_t minMHz=sampleMHz<s_last_good[core].cpuMHz?sampleMHz:s_last_good[core].cpuMHz;
                if(!zonlineSystickTimePlausible(((uint64_t)hi<<32)|lo,
                    ((uint64_t)s_last_good[core].hi<<32)|s_last_good[core].lo,
                    period,sampleCycles-s_last_good[core].cycles,minMHz)) reason=8;
            }
        }
        if(reason==1 || reason==2 || reason==4 || reason==8) {
            const unsigned originalReason=reason;
            s_recovery[core].attempts++;
            if(reason==1) s_recovery[core].reason1++;
            if(reason==2) s_recovery[core].reason2++;
            if(reason==4) s_recovery[core].reason4++;
            if(reason==8) s_recovery[core].reason8++;
            if(period==16000000U/CONFIG_FREERTOS_HZ &&
                zonlineSystickTryRecover(hal->dev,core,period,handledBefore,started,
                    &lo,&hi,&pending,&polls,&retries,&sampleCycles,&sampleMHz)) {
                s_recovery[core].recovered++;
                reason=0;
            } else {
                s_recovery[core].rejected++;
                reason=originalReason; goto fault;
            }
        }
        if(reason) goto fault;
        if(pending) {
            s_handled_systicks[core]+=pending;
            s_handled_inverse[core]=~s_handled_systicks[core];
            if(!s_initialized[core]) {s_initialized[core]=1;pending=1;}
            do {
                xPortSysTickHandler();
                if((uint32_t)(esp_cpu_get_cycle_count()-started)>ZONLINE_SYSTICK_MAX_CYCLES) {reason=5;goto fault;}
            } while(--pending);
        }
        s_last_good[core].lo=lo; s_last_good[core].hi=hi;
        s_last_good[core].cycles=sampleCycles;
        s_last_good[core].cpuMHz=sampleMHz;
        s_last_good[core].handled=s_handled_systicks[core];
        if(++rounds>=ZONLINE_SYSTICK_MAX_ROUNDS) {reason=6;goto fault;}
    } while(systimer_ll_is_alarm_int_fired(hal->dev,alarm));
#ifdef CONFIG_PM_TRACE
    ESP_PM_TRACE_EXIT(TICK, core);
#endif
    return;
fault:
    zonlineTimerFault(core,reason,period,handledBefore,pending,lo,hi,opBefore,polls,retries);
    zonlineSystickEmergency(core); // arm reset before any additional fault-only sampling
    zonlineSystimerProbeBegin(core,lo,hi); // read-only pre-confirm registers; fault path only
    zonlineSystimerProbeRecovery(core,s_recovery[core].attempts,
        s_recovery[core].recovered,s_recovery[core].rejected);
    {
        uint32_t checkLo=0,checkHi=0,checkPolls=0,checkRetries=0;
        const unsigned checkReason=zonlineSystickReadBounded(hal->dev,&checkLo,&checkHi,&checkPolls,&checkRetries);
        // packed: reason[7:0], retries[15:8], polls[28:16], detail[30], initialized[31]
        const uint32_t meta=(checkReason&255U)|((checkRetries&255U)<<8)|((checkPolls&8191U)<<16)|
            0x40000000U|(s_initialized[core]?0x80000000U:0U);
        zonlineTimerFaultDetail(core,s_last_good[core].lo,s_last_good[core].hi,
            s_last_good[core].cycles,s_last_good[core].handled,checkLo,checkHi,meta);
        zonlineSystimerProbeFinish(core,checkLo,checkHi,meta);
    }
    // Accounting is suspect: never continue with a clipped/fabricated counter.
    // Higher-priority IWDT and the independent RTC reset remain enabled.
    for(;;) __asm__ __volatile__("memw" ::: "memory");
}
#endif /* CONFIG_FREERTOS_SYSTICK_USES_SYSTIMER */

/* ------------------------------------------------ Common Port Tick ---------------------------------------------------
 * Tick related functions common across all ports
 * ------------------------------------------------------------------------------------------------------------------ */

/**
 * @brief Initialize the tick interrupt timer
 *
 * - CCOUNT timer is used if CONFIG_FREERTOS_SYSTICK_USES_CCOUNT is set
 * - SYSTIMER is used if CONFIG_FREERTOS_SYSTICK_USES_SYSTIMER is set
 */
void vPortSetupTimer(void)
{
#if CONFIG_FREERTOS_SYSTICK_USES_CCOUNT
    extern void _frxt_tick_timer_init(void);
    extern void _xt_tick_divisor_init(void);
    /* Init the tick divisor value */
    _xt_tick_divisor_init();
    _frxt_tick_timer_init();
#else /* CONFIG_FREERTOS_SYSTICK_USES_SYSTIMER */
    vSystimerSetup();
#endif /* CONFIG_FREERTOS_SYSTICK_USES_SYSTIMER */
}

/**
 * @brief Handler of SysTick
 *
 * The function is called from:
 *  - _frxt_timer_int for xtensa with CONFIG_FREERTOS_SYSTICK_USES_CCOUNT
 *  - SysTickIsrHandler for xtensa with CONFIG_FREERTOS_SYSTICK_USES_SYSTIMER
 *  - SysTickIsrHandler for riscv
 */
BaseType_t IRAM_ATTR xPortSysTickHandler(void)
{
#if configBENCHMARK
    portbenchmarkIntLatency();
#endif //configBENCHMARK
    traceISR_ENTER(SYSTICK_INTR_ID);

    // Call IDF Tick Hook
    extern void esp_vApplicationTickHook(void);
    esp_vApplicationTickHook();

    // Call FreeRTOS Increment tick function
    BaseType_t xSwitchRequired;
#if CONFIG_FREERTOS_SMP
    UBaseType_t uxSavedStatus = taskENTER_CRITICAL_FROM_ISR();
    // Amazon SMP FreeRTOS requires that only core 0 calls xTaskIncrementTick()
#if ( configNUM_CORES > 1 )
    if (portGET_CORE_ID() == 0) {
        xSwitchRequired = xTaskIncrementTick();
    } else {
        xSwitchRequired = pdFALSE;
    }
#else /* configNUM_CORES > 1 */
    xSwitchRequired = xTaskIncrementTick();
#endif /* configNUM_CORES > 1 */
    taskEXIT_CRITICAL_FROM_ISR(uxSavedStatus);
#else /* !CONFIG_FREERTOS_SMP */
#if ( configNUM_CORES > 1 )
    /*
    Multi-core IDF FreeRTOS requires that...
        - core 0 calls xTaskIncrementTick()
        - core 1 calls xTaskIncrementTickOtherCores()
    */
    if (xPortGetCoreID() == 0) {
        xSwitchRequired = xTaskIncrementTick();
    } else {
        xSwitchRequired = xTaskIncrementTickOtherCores();
    }
#else /* configNUM_CORES > 1 */
    /*
    Vanilla (single core) FreeRTOS expects that xTaskIncrementTick() cannot be interrupted (i.e., no nested
    interrupts). Thus we have to disable interrupts before calling it.
    */
    UBaseType_t uxSavedInterruptStatus = portSET_INTERRUPT_MASK_FROM_ISR();
    xSwitchRequired = xTaskIncrementTick();
    portCLEAR_INTERRUPT_MASK_FROM_ISR(uxSavedInterruptStatus);
#endif /* configNUM_CORES > 1 */
#endif /* !CONFIG_FREERTOS_SMP */

    // Check if yield is required
    if (xSwitchRequired != pdFALSE) {
        portYIELD_FROM_ISR();
    } else {
        traceISR_EXIT();
    }
    return xSwitchRequired;
}
