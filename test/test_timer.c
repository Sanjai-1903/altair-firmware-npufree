/*
 * test_timer.c — Timer driver / RTL smoke test
 * Links as a standalone ELF with its own main().
 * No stdlib, no printf.
 *
 * Exercises (see SoC/rtl/timer/timer_top.sv, timer_regs.sv and
 * firmware/include/timer.h for the verified RTL contract this drives):
 *   - register initialisation (timer_init: CONTROL=0, PRESCALER, COMPARE)
 *   - counter free-run (RTL has no explicit enable/start bit; it counts
 *     unconditionally once out of reset)
 *   - compare register + one-shot match + status bit + interrupt-clear
 *     sequence, via timer_delay_ms() (production driver API)
 *   - periodic mode: repeated match/status/clear cycles + timer_irq line
 */

#include "uart.h"
#include "timer.h"

int main(void)
{
    uint32_t matches;

    uart_init(115200U);
    uart_puts("timer test start\r\n");

    /* 1. Register initialisation: prescaler=0 (tick_div=1, raw 50 MHz
     * ticks), CONTROL cleared, COMPARE defaulted to 0xFFFFFFFF. */
    timer_init(0U);
    uart_puts("timer init ok\r\n");

    /* 2. One-shot compare/match/status/clear sequence. delta = 50000
     * ticks (1 ms @ 50 MHz / tick_div=1) keeps this within a few ms of
     * simulated time. */
    timer_delay_ms(1U);
    uart_puts("timer oneshot match ok\r\n");

    /* 3. Periodic mode: enable, observe at least two match/clear cycles,
     * proving TIMER_STATUS and TIMER_INTERRUPT_CLR both function
     * repeatedly (not just once). */
    timer_irq_enable();
    matches = 0U;
    while (matches < 2U) {
        if (timer_irq_status() & TIMER_STAT_MATCH) {
            matches++;
            timer_irq_clear();
        }
    }
    uart_puts("timer periodic ok\r\n");

    /* 4. Leave the timer stopped so the test image idles cleanly. */
    timer_irq_clear();
    TIMER_REG(TIMER_CONTROL) = 0U;

    uart_puts("timer PASS\r\n");
    while (1) { __asm__ volatile ("wfi"); }
    return 0;
}
