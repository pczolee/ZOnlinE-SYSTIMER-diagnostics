/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include "esp_attr.h"
#include "esp_cpu.h"
#include "zonline_diagnostic_sdk_guard.h"
#include "zonline_fault_capture.h"
#include <sys/param.h>
#include "soc/soc_caps.h"
#include "hal/systimer_hal.h"
#include "hal/systimer_ll.h"
#include "hal/systimer_types.h"
#include "hal/assert.h"
#include "zonline_systick_read.h"

// Full IDF 5.5.2 HAL object override: all public symbols below must stay together
// so the SDK archive does not pull a second copy of systimer_hal.o. There are
// no special linker flags. All entry points are IRAM, as the SDK linker placed
// the original HAL in IRAM too. Only counter reads differ from upstream.
extern void zonlineSystickEmergency(unsigned core);

static void IRAM_ATTR __attribute__((noreturn)) timerReadFailed(unsigned unit,
    unsigned reason,uint32_t lo,uint32_t hi,uint32_t polls,uint32_t retries) {
    const unsigned core=esp_cpu_get_core_id();
    // No scheduler lock, esp_timer, heap, logging or flash on the failure path.
    // Mask ordinary interrupts, preserving the higher-priority interrupt WDT.
    unsigned ps;
    __asm__ __volatile__("rsil %0, 3" : "=a"(ps) :: "memory");
    (void)ps;
    zonlineSystickEmergency(core);
    zonlineTimerReadFault(core,unit,reason,lo,hi,polls,retries);
    for(;;) __asm__ __volatile__("memw" ::: "memory");
}


void IRAM_ATTR systimer_hal_init(systimer_hal_context_t *hal)
{
    hal->dev = &SYSTIMER;
    systimer_ll_enable_clock(hal->dev, true);
#if SOC_SYSTIMER_SUPPORT_ETM
    systimer_ll_enable_etm(&SYSTIMER, true);
#endif
}

void IRAM_ATTR systimer_hal_deinit(systimer_hal_context_t *hal)
{
#if SOC_SYSTIMER_SUPPORT_ETM
    systimer_ll_enable_etm(&SYSTIMER, false);
#endif
    systimer_ll_enable_clock(hal->dev, false);
    hal->dev = NULL;
}

systimer_clock_source_t IRAM_ATTR systimer_hal_get_clock_source(systimer_hal_context_t *hal)
{
    (void)hal;
    return systimer_ll_get_clock_source();
}

void IRAM_ATTR systimer_hal_set_tick_rate_ops(systimer_hal_context_t *hal, systimer_hal_tick_rate_ops_t *ops)
{
    hal->ticks_to_us = ops->ticks_to_us;
    hal->us_to_ticks = ops->us_to_ticks;
}

uint64_t IRAM_ATTR systimer_hal_get_counter_value(systimer_hal_context_t *hal, uint32_t counter_id)
{
    uint32_t lo=0,hi=0,polls=0,retries=0;
    if(counter_id>1U) timerReadFailed(counter_id,3U,lo,hi,polls,retries);
    const unsigned reason=zonlineSystimerReadBounded(hal->dev,counter_id,&lo,&hi,&polls,&retries);
    if(reason) timerReadFailed(counter_id,reason,lo,hi,polls,retries);
    return ((uint64_t)hi<<32)|lo;
}

uint64_t IRAM_ATTR systimer_hal_get_time(systimer_hal_context_t *hal, uint32_t counter_id)
{
    return hal->ticks_to_us(systimer_hal_get_counter_value(hal, counter_id));
}

#if SOC_SYSTIMER_ALARM_MISS_COMPENSATE
void IRAM_ATTR systimer_hal_set_alarm_target(systimer_hal_context_t *hal, uint32_t alarm_id, uint64_t target)
{
    systimer_counter_value_t alarm = {
        .val = hal->us_to_ticks(target),
    };
    systimer_ll_enable_alarm(hal->dev, alarm_id, false);
    systimer_ll_set_alarm_target(hal->dev, alarm_id, alarm.val);
    systimer_ll_apply_alarm_value(hal->dev, alarm_id);
    systimer_ll_enable_alarm(hal->dev, alarm_id, true);
}

#else // SOC_SYSTIMER_ALARM_MISS_COMPENSATE

void IRAM_ATTR systimer_hal_set_alarm_target(systimer_hal_context_t *hal, uint32_t alarm_id, uint64_t timestamp)
{
    int64_t offset = hal->us_to_ticks(1) * 2;
    uint64_t now_time = systimer_hal_get_counter_value(hal, 0);
    systimer_counter_value_t alarm = { .val = MAX(hal->us_to_ticks(timestamp), now_time + offset) };
    do {
        systimer_ll_enable_alarm(hal->dev, alarm_id, false);
        systimer_ll_set_alarm_target(hal->dev, alarm_id, alarm.val);
        systimer_ll_enable_alarm(hal->dev, alarm_id, true);
        now_time = systimer_hal_get_counter_value(hal, 0);
        int64_t delta = (int64_t)alarm.val - (int64_t)now_time;
        if (delta <= 0 && !systimer_ll_is_alarm_int_fired(hal->dev, alarm_id)) {
            // new alarm is less than the counter and the interrupt flag is not set
            offset += -1 * delta + hal->us_to_ticks(1) * 2;
            alarm.val = now_time + offset;
        } else {
            // finish if either (alarm > counter) or the interrupt flag is already set.
            break;
        }
    } while (1);
}
#endif // SOC_SYSTIMER_ALARM_MISS_COMPENSATE

void IRAM_ATTR systimer_hal_set_alarm_period(systimer_hal_context_t *hal, uint32_t alarm_id, uint32_t period)
{
    systimer_ll_enable_alarm(hal->dev, alarm_id, false);
    systimer_ll_set_alarm_period(hal->dev, alarm_id, hal->us_to_ticks(period));
    systimer_ll_apply_alarm_value(hal->dev, alarm_id);
    systimer_ll_enable_alarm(hal->dev, alarm_id, true);
}

uint64_t IRAM_ATTR systimer_hal_get_alarm_value(systimer_hal_context_t *hal, uint32_t alarm_id)
{
    return systimer_ll_get_alarm_target(hal->dev, alarm_id);
}

void IRAM_ATTR systimer_hal_enable_alarm_int(systimer_hal_context_t *hal, uint32_t alarm_id)
{
    systimer_ll_enable_alarm_int(hal->dev, alarm_id, true);
}

void IRAM_ATTR systimer_hal_counter_value_advance(systimer_hal_context_t *hal, uint32_t counter_id, int64_t time_us)
{
    systimer_counter_value_t new_count = {
        .val = systimer_hal_get_counter_value(hal, counter_id) + hal->us_to_ticks(time_us),
    };
    systimer_ll_set_counter_value(hal->dev, counter_id, new_count.val);
    systimer_ll_apply_counter_value(hal->dev, counter_id);
}

void IRAM_ATTR systimer_hal_enable_counter(systimer_hal_context_t *hal, uint32_t counter_id)
{
    systimer_ll_enable_counter(hal->dev, counter_id, true);
}

void IRAM_ATTR systimer_hal_select_alarm_mode(systimer_hal_context_t *hal, uint32_t alarm_id, systimer_alarm_mode_t mode)
{
    switch (mode) {
    case SYSTIMER_ALARM_MODE_ONESHOT:
        systimer_ll_enable_alarm_oneshot(hal->dev, alarm_id);
        break;
    case SYSTIMER_ALARM_MODE_PERIOD:
        systimer_ll_enable_alarm_period(hal->dev, alarm_id);
        break;
    default:
        break;
    }
}

void IRAM_ATTR systimer_hal_connect_alarm_counter(systimer_hal_context_t *hal, uint32_t alarm_id, uint32_t counter_id)
{
    systimer_ll_connect_alarm_counter(hal->dev, alarm_id, counter_id);
}

void IRAM_ATTR systimer_hal_counter_can_stall_by_cpu(systimer_hal_context_t *hal, uint32_t counter_id, uint32_t cpu_id, bool can)
{
    systimer_ll_counter_can_stall_by_cpu(hal->dev, counter_id, cpu_id, can);
}

#if !SOC_SYSTIMER_FIXED_DIVIDER

void IRAM_ATTR systimer_hal_set_steps_per_tick(systimer_hal_context_t *hal, int clock_source, uint32_t steps)
{
    /* Configure the counter:
     * - increment by 1 when running from PLL (80 ticks per microsecond),
     * - increment by 2 when running from XTAL (40 ticks per microsecond).
     * Note that if the APB frequency is derived from XTAL with divider != 1,
     * XTAL_STEP needs to be adjusted accordingly. For example, if
     * the APB frequency is XTAL/4 = 10 MHz, then XTAL_STEP should be set to 8.
     * This is handled in systimer_hal_on_apb_freq_update function.
     */
    switch (clock_source) {
    case 0:
        systimer_ll_set_step_for_xtal(hal->dev, steps);
        break;
    case 1:
        systimer_ll_set_step_for_pll(hal->dev, steps);
    default:
        break;
    }
}

void IRAM_ATTR systimer_hal_on_apb_freq_update(systimer_hal_context_t *hal, uint32_t apb_ticks_per_us)
{
    /* If this function was called when switching APB clock to PLL, don't need
    * do anything: the SYSTIMER_TIMER_PLL_STEP is already correct.
    * If this was called when switching APB clock to XTAL, need to adjust
    * XTAL_STEP value accordingly.
    */
    if (apb_ticks_per_us != hal->us_to_ticks(1)) {
        HAL_ASSERT((hal->us_to_ticks(1) % apb_ticks_per_us) == 0 && "TICK_PER_US should be divisible by APB frequency (in MHz)");
        systimer_ll_set_step_for_xtal(hal->dev, hal->us_to_ticks(1) / apb_ticks_per_us);
    }
}
#endif // !SOC_SYSTIMER_FIXED_DIVIDER

