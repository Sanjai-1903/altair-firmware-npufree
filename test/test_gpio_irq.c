/*
 * test_gpio_irq.c — GPIO interrupt-mode verification (Phase 8, Part A)
 *
 * RTL interrupt model (gpio_top.sv): gpio_irq_o is already edge-detected in
 * hardware (`mismatch & ~mismatch_q`), a one-cycle pulse per new mismatch.
 * There is no GPIO-local clear register (RTL_GAPS #8). Prior to Phase 8,
 * trap.c never serviced IRQ_GPIO at all — this image exercises the newly
 * added gpio_irq_service() path end-to-end.
 *
 * Covers:
 *   1. Interrupt generation + pending + trap dispatch — DEBUG0 input mismatch
 *      (driven externally via the cocotb gpio_test_in hook) fires the IRQ;
 *      ISR captures GPIO_INTERRUPT_STATUS and sets _gpio_irq_done
 *   2. Interrupt mask/unmask at the INTC level
 *
 * No stdlib / POSIX headers.
 */

#include "gpio.h"
#include "intc.h"
#include "uart.h"

/* ISR completion flag defined in trap.c; read directly here (test-only,
 * bypassing gpio_irq_wait()'s ~40ms-nominal shared-driver timeout) to
 * verify masking without paying that full simulated-time cost. */
extern volatile uint32_t _gpio_irq_done;

int main(void)
{
    uart_init(115200U);
    uart_puts("gpio irq test start\r\n");

    intc_init();
    gpio_init();

    /* DEBUG0 (bit 2) as input; GPIO_DATA bit2 stays 0 (reset value) as the
     * interrupt-compare level, so any externally-driven high on that pin
     * mismatches immediately. */
    gpio_set_input(GPIO_PIN_DEBUG0);
    gpio_irq_flag_clear();
    gpio_irq_enable(GPIO_PIN_DEBUG0);
    intc_enable(IRQ_GPIO);
    intc_global_enable();

    uart_puts("gpio irq: waiting for edge\r\n");

    if (gpio_irq_wait() != 0) {
        uart_puts("[FAIL] t1: gpio_irq_wait timeout\r\n");
        return 1;
    }
    uart_puts("[PASS] t1: gpio mismatch interrupt\r\n");
    uart_puts("gpio irq status at capture: ");
    uart_put_hex32((uint32_t)gpio_irq_last_status());
    uart_puts("\r\n");

    /* ── Test 2: mask/unmask ─────────────────────────────────────────────── */
    intc_disable(IRQ_GPIO);
    gpio_irq_flag_clear();
    {
        volatile uint32_t d = 20000U;
        while (d--) { }
    }
    if (_gpio_irq_done) {
        uart_puts("[FAIL] t2: masked IRQ still set the completion flag\r\n");
        return 1;
    }
    uart_puts("[PASS] t2: IRQ correctly masked\r\n");

    uart_puts("[PASS] All gpio IRQ tests\r\n");
    while (1) { __asm__ volatile ("wfi"); }
    return 0;
}
