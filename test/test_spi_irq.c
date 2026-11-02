/*
 * test_spi_irq.c — SPI interrupt-mode verification (Phase 8, Part A)
 *
 * RTL interrupt model (spi_top.sv): spi_irq_o is a one-cycle completion
 * pulse (`CONTROL[0] & spi_done_pulse`, busy-falling-edge), no peripheral-
 * side latch to clear. Prior to Phase 8, trap.c never serviced IRQ_SPI at
 * all — this image exercises the newly added spi_irq_service() path.
 *
 * Covers:
 *   1. Completion interrupt — TX write launches a transfer; IRQ fires on
 *      completion; ISR sets _spi_irq_done
 *   2. Interrupt mask/unmask at the INTC level (transfer still completes in
 *      hardware while masked — polled via spi_busy())
 *   3. Back-to-back: a third transfer after re-enabling confirms mtvec/
 *      enable state is persistent, not a one-shot artifact
 *
 * spi_miso_i is left at its reset default (0) — this test only verifies
 * the interrupt/timing contract, not MISO data content (already covered
 * by test_spi.py in Phase 7).
 *
 * No stdlib / POSIX headers.
 */

#include "spi.h"
#include "intc.h"
#include "uart.h"

/* ISR completion flag defined in trap.c; read directly here (test-only,
 * bypassing spi_irq_wait()'s ~40ms-nominal shared-driver timeout) to
 * verify masking without paying that full simulated-time cost. */
extern volatile uint32_t _spi_irq_done;

int main(void)
{
    uart_init(115200U);
    uart_puts("spi irq test start\r\n");

    intc_init();
    spi_init(4U);

    /* ── Test 1: completion interrupt ───────────────────────────────────── */
    uart_puts("spi irq test 1: completion interrupt\r\n");

    spi_irq_flag_clear();
    spi_irq_enable();
    intc_enable(IRQ_SPI);
    intc_global_enable();

    SPI_REG(SPI_REG_TX) = 0xA5A5A5A5UL;

    if (spi_irq_wait() != 0) {
        uart_puts("[FAIL] t1: spi_irq_wait timeout\r\n");
        return 1;
    }
    uart_puts("[PASS] t1: spi completion interrupt\r\n");

    /* ── Test 2: mask/unmask ─────────────────────────────────────────────── */
    uart_puts("spi irq test 2: mask/unmask\r\n");

    intc_disable(IRQ_SPI);
    spi_irq_flag_clear();
    SPI_REG(SPI_REG_TX) = 0x12345678UL;
    while (spi_busy()) { }   /* let the masked transfer actually finish */
    if (_spi_irq_done) {
        uart_puts("[FAIL] t2: masked IRQ still set the completion flag\r\n");
        return 1;
    }
    uart_puts("[PASS] t2: IRQ correctly masked\r\n");

    /* ── Test 3: re-enable / back-to-back ───────────────────────────────────── */
    uart_puts("spi irq test 3: re-enable (back-to-back)\r\n");

    intc_enable(IRQ_SPI);
    spi_irq_flag_clear();
    SPI_REG(SPI_REG_TX) = 0x87654321UL;
    if (spi_irq_wait() != 0) {
        uart_puts("[FAIL] t3: re-enabled IRQ did not fire\r\n");
        return 1;
    }
    uart_puts("[PASS] t3: IRQ correctly unmasked\r\n");

    uart_puts("[PASS] All spi IRQ tests\r\n");
    while (1) { __asm__ volatile ("wfi"); }
    return 0;
}
