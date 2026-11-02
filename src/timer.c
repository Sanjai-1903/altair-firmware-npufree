/*
 * timer.c - Timer driver aligned to current RTL
 * No stdlib / POSIX headers.
 */

#include "timer.h"

/* ISR completion flag defined in trap.c; written only from M-mode trap context */
extern volatile uint32_t _timer_irq_done;

/* Effective tick divider of the timer: prescaler register + 1. */
static uint32_t g_tick_div = 1U;

/* Rolling watchdog timeout in timer ticks. */
static uint32_t g_watchdog_ticks = 0xFFFFFFFFUL;

static uint32_t timer_ticks_from_ms(uint32_t ms)
{
    return (SYS_CLK_HZ / g_tick_div) / 1000U * ms;
}

void timer_init(uint32_t prescaler)
{
    g_tick_div = prescaler + 1U;

    TIMER_REG(TIMER_CONTROL) = 0U;
    TIMER_REG(TIMER_INTERRUPT_CLR) = 1U;
    TIMER_REG(TIMER_PRESCALER) = prescaler;
    TIMER_REG(TIMER_COMPARE) = 0xFFFFFFFFUL;
}

void timer_start(void)
{
    /* RTL has no explicit start bit; leave the current mode unchanged. */
}

void timer_stop(void)
{
    /* RTL exposes no stop gate; keep this as a compatibility no-op. */
}

void timer_set_compare(uint32_t cmp)
{
    TIMER_REG(TIMER_COMPARE) = cmp;
}

uint32_t timer_get_value(void)
{
    return TIMER_REG(TIMER_VALUE);
}

void timer_delay_ms(uint32_t ms)
{
    uint32_t now;
    uint32_t delta;

    delta = timer_ticks_from_ms(ms);
    now = TIMER_REG(TIMER_VALUE);

    TIMER_REG(TIMER_CONTROL) = 0U;
    TIMER_REG(TIMER_INTERRUPT_CLR) = 1U;
    TIMER_REG(TIMER_COMPARE) = now + delta;
    TIMER_REG(TIMER_CONTROL) = TIMER_CTRL_ONE_SHOT;

    while (!(TIMER_REG(TIMER_STATUS) & TIMER_STAT_MATCH))
        ;

    TIMER_REG(TIMER_INTERRUPT_CLR) = 1U;
    TIMER_REG(TIMER_CONTROL) = 0U;
}

void timer_irq_enable(void)
{
    TIMER_REG(TIMER_INTERRUPT_CLR) = 1U;
    TIMER_REG(TIMER_CONTROL) = TIMER_CTRL_PERIODIC;
}

void timer_irq_clear(void)
{
    TIMER_REG(TIMER_INTERRUPT_CLR) = 1U;
}

uint32_t timer_irq_status(void)
{
    return TIMER_REG(TIMER_STATUS);
}

void watchdog_init(uint32_t timeout_ms)
{
    uint32_t now;

    g_watchdog_ticks = timer_ticks_from_ms(timeout_ms);
    now = TIMER_REG(TIMER_VALUE);

    TIMER_REG(TIMER_CONTROL) = 0U;
    TIMER_REG(TIMER_INTERRUPT_CLR) = 1U;
    TIMER_REG(TIMER_COMPARE) = now + g_watchdog_ticks;
    TIMER_REG(TIMER_CONTROL) = TIMER_CTRL_ONE_SHOT;
}

void watchdog_kick(void)
{
    uint32_t now;

    now = TIMER_REG(TIMER_VALUE);
    TIMER_REG(TIMER_INTERRUPT_CLR) = 1U;
    TIMER_REG(TIMER_COMPARE) = now + g_watchdog_ticks;
    TIMER_REG(TIMER_CONTROL) = TIMER_CTRL_ONE_SHOT;
}

int watchdog_expired(void)
{
    return (TIMER_REG(TIMER_STATUS) & TIMER_STAT_MATCH) ? 1 : 0;
}

void timer_irq_flag_clear(void)
{
    _timer_irq_done = 0U;
}

int timer_irq_wait(void)
{
    uint32_t timeout = 0x100000UL;   /* ~2M cycles @ 50 MHz ~= 40 ms */

    while (!_timer_irq_done) {
        if (--timeout == 0U)
            return -1;
    }
    return 0;
}
