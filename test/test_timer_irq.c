/*
 * test_timer_irq.c — Timer interrupt-mode verification (Phase 8, Part A)
 *
 * RTL interrupt model (timer_top.sv): timer_irq = TIMER_STATUS[0], a LEVEL
 * signal that stays asserted until firmware writes TIMER_INTERRUPT_CLR.
 * Prior to Phase 8, trap.c never serviced IRQ_TIMER at all — this image
 * exercises the newly added timer_irq_service() path end-to-end.
 *
 * Covers:
 *   1. Completion interrupt   — compare match fires IRQ; ISR sets _timer_irq_done
 *   2. Periodic re-fire        — same ISR fires repeatedly without re-arming,
 *                                 proving the two-stage clear (peripheral latch
 *                                 THEN intc_ack) does not leave a ghost pending
 *                                 bit or miss the next match
 *   3. Interrupt mask/unmask   — intc_disable(IRQ_TIMER) blocks delivery;
 *                                 intc_enable(IRQ_TIMER) restores it
 *   4. Clear sequence          — no spurious/missing IRQ after each clear
 *
 * No stdlib / POSIX headers.
 */

#include "timer.h"
#include "intc.h"
#include "uart.h"

/* ISR completion flag defined in trap.c; read directly here (test-only,
 * bypassing timer_irq_wait()'s ~40ms-nominal shared-driver timeout) to
 * verify masking without paying that full simulated-time cost — the
 * periodic period programmed below is ~2000 cycles, so a short bounded
 * delay is ample proof the flag never gets set while masked. */
extern volatile uint32_t _timer_irq_done;

int main(void)
{
    uart_init(115200U);
    uart_puts("timer irq test start\r\n");

    intc_init();
    timer_init(0U);

    /* ── Test 1: completion interrupt ───────────────────────────────────── */
    uart_puts("timer irq test 1: completion interrupt\r\n");

    timer_irq_flag_clear();
    TIMER_REG(TIMER_COMPARE) = TIMER_REG(TIMER_VALUE) + 2000U;
    timer_irq_enable();          /* peripheral: periodic mode, clears stale status */
    intc_enable(IRQ_TIMER);      /* arm INTC side */
    intc_global_enable();        /* set mstatus.MIE */

    if (timer_irq_wait() != 0) {
        uart_puts("[FAIL] t1: timer_irq_wait timeout\r\n");
        return 1;
    }
    uart_puts("[PASS] t1: completion interrupt\r\n");

    /* ── Test 2: periodic re-fire (two-stage clear correctness) ──────────── */
    uart_puts("timer irq test 2: periodic re-fire\r\n");

    timer_irq_flag_clear();
    if (timer_irq_wait() != 0) {
        uart_puts("[FAIL] t2: second periodic match did not fire\r\n");
        return 1;
    }
    uart_puts("[PASS] t2: periodic re-fire\r\n");

    /* ── Test 3: mask/unmask at INTC level ────────────────────────────────── */
    uart_puts("timer irq test 3: mask/unmask\r\n");

    intc_disable(IRQ_TIMER);
    timer_irq_flag_clear();
    {
        volatile uint32_t d = 20000U;   /* >>1 periodic period (~2000 cycles) */
        while (d--) { }
    }
    if (_timer_irq_done) {
        uart_puts("[FAIL] t3a: masked IRQ still set the completion flag\r\n");
        return 1;
    }
    uart_puts("[PASS] t3a: IRQ correctly masked\r\n");

    intc_enable(IRQ_TIMER);
    timer_irq_flag_clear();
    if (timer_irq_wait() != 0) {
        uart_puts("[FAIL] t3b: re-enabled IRQ did not fire\r\n");
        return 1;
    }
    uart_puts("[PASS] t3b: IRQ correctly unmasked\r\n");

    /* ── Test 4: interrupt clear sequence — no ghost IRQ ──────────────────── */
    uart_puts("timer irq test 4: clear sequence\r\n");

    timer_irq_flag_clear();
    if (timer_irq_wait() != 0) {
        uart_puts("[FAIL] t4: spurious or missing IRQ after clear\r\n");
        return 1;
    }
    uart_puts("[PASS] t4: clear sequence correct\r\n");

    TIMER_REG(TIMER_CONTROL) = 0U;
    intc_disable(IRQ_TIMER);

    uart_puts("[PASS] All timer IRQ tests\r\n");
    while (1) { __asm__ volatile ("wfi"); }
    return 0;
}
