/*
 * test_i2c_irq.c — I2C interrupt-mode verification (Phase 8, Part A)
 *
 * RTL interrupt model (i2c_top.sv): i2c_irq_o is a one-cycle completion
 * pulse (`CONTROL[0] & i2c_done_pulse`, busy-falling-edge; fires regardless
 * of ACK/NACK outcome), no peripheral-side latch to clear. Prior to
 * Phase 8, trap.c never serviced IRQ_I2C at all — this image exercises the
 * newly added i2c_irq_service() path.
 *
 * Covers:
 *   1. Completion interrupt — TX write launches the fixed START/ADDR/ACK/
 *      DATA/NACK/STOP sequence; IRQ fires on completion; ISR sets
 *      _i2c_irq_done
 *   2. Interrupt mask/unmask at the INTC level (transaction still completes
 *      in hardware while masked — polled via i2c_busy())
 *   3. Back-to-back: a third transaction after re-enabling confirms mtvec/
 *      enable state is persistent
 *
 * i2c_sda_test_in is left at its reset default (0 = a slave that ACKs, per
 * common.py's reset_dut) — this test only verifies the interrupt/timing
 * contract, not ACK/NACK data content (already covered by test_i2c.py in
 * Phase 7).
 *
 * No stdlib / POSIX headers.
 */

#include "i2c.h"
#include "intc.h"
#include "uart.h"

/* ISR completion flag defined in trap.c; read directly here (test-only,
 * bypassing i2c_irq_wait()'s ~40ms-nominal shared-driver timeout) to
 * verify masking without paying that full simulated-time cost. */
extern volatile uint32_t _i2c_irq_done;

int main(void)
{
    uart_init(115200U);
    uart_puts("i2c irq test start\r\n");

    intc_init();
    i2c_init(9U);

    /* ── Test 1: completion interrupt ───────────────────────────────────── */
    uart_puts("i2c irq test 1: completion interrupt\r\n");

    i2c_irq_flag_clear();
    i2c_irq_enable();
    intc_enable(IRQ_I2C);
    intc_global_enable();

    I2C_REG(I2C_REG_TX) = 0x000000AAUL;

    if (i2c_irq_wait() != 0) {
        uart_puts("[FAIL] t1: i2c_irq_wait timeout\r\n");
        return 1;
    }
    uart_puts("[PASS] t1: i2c completion interrupt\r\n");

    /* ── Test 2: mask/unmask ─────────────────────────────────────────────── */
    uart_puts("i2c irq test 2: mask/unmask\r\n");

    intc_disable(IRQ_I2C);
    i2c_irq_flag_clear();
    I2C_REG(I2C_REG_TX) = 0x000000BBUL;
    while (i2c_busy()) { }   /* let the masked transaction actually finish */
    if (_i2c_irq_done) {
        uart_puts("[FAIL] t2: masked IRQ still set the completion flag\r\n");
        return 1;
    }
    uart_puts("[PASS] t2: IRQ correctly masked\r\n");

    /* ── Test 3: re-enable / back-to-back ───────────────────────────────────── */
    uart_puts("i2c irq test 3: re-enable (back-to-back)\r\n");

    intc_enable(IRQ_I2C);
    i2c_irq_flag_clear();
    I2C_REG(I2C_REG_TX) = 0x000000CCUL;
    if (i2c_irq_wait() != 0) {
        uart_puts("[FAIL] t3: re-enabled IRQ did not fire\r\n");
        return 1;
    }
    uart_puts("[PASS] t3: IRQ correctly unmasked\r\n");

    uart_puts("[PASS] All i2c IRQ tests\r\n");
    while (1) { __asm__ volatile ("wfi"); }
    return 0;
}
