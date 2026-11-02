/*
 * test_uart_rx.c — UART receive-path verification (Phase 8, Part A + Part C)
 *
 * Production firmware never reads UART RX (uart_getc()/uart_rx_ready() are
 * unused by main.c), and trap.c never serviced IRQ_UART before Phase 8.
 * This image is the first dynamic exercise of uart_rx.sv and the RX-done
 * interrupt path.
 *
 * RTL contract (uart_rx.sv, uart_top.sv):
 *   - UART_RX holds only the single most-recently-received byte; no FIFO.
 *   - rx_done pulses for exactly one cycle when a byte finishes; gated by
 *     UART_CONTROL[1] it drives uart_irq_o (combined with the TX-done pulse
 *     on the same line — RTL_GAPS #6, no per-source status bit).
 *   - uart_rx.sv does not check the stop bit's line level at all (no
 *     framing-error detection) and has no parity support — RTL NOT
 *     IMPLEMENTED, not exercised here (nothing to test).
 *
 * Covers:
 *   1. RX-done interrupt fires and captures the correct byte in UART_RX
 *   2. Multiple sequentially received bytes (RTL has no FIFO — each must be
 *      drained by the ISR before the next byte's done pulse, which this
 *      test's slow byte-at-a-time cadence guarantees)
 *   3. Trap dispatch / ack sequence via the same INTC path as every other
 *      Phase 8 interrupt source
 *
 * Each received byte is echoed back over TX so the cocotb bench can verify
 * captured content non-invasively via the existing UartMonitor.
 *
 * No stdlib / POSIX headers.
 */

#include "uart.h"
#include "intc.h"

int main(void)
{
    uint32_t i;

    uart_init(115200U);
    uart_puts("uart rx test start\r\n");

    intc_init();
    uart_irq_flag_clear();
    uart_irq_enable(UART_CTRL_IRQ_RX_DONE);
    intc_enable(IRQ_UART);
    intc_global_enable();

    uart_puts("uart rx: waiting\r\n");

    for (i = 0U; i < 3U; i++) {
        if (uart_irq_wait_rx() != 0) {
            uart_puts("[FAIL] uart rx: timeout waiting for byte\r\n");
            return 1;
        }
        {
            uint8_t b = uart_irq_get_rx_byte();
            uart_irq_flag_clear();
            uart_puts("uart rx byte: ");
            uart_put_hex32((uint32_t)b);
            uart_puts("\r\n");
            /* Echo it back so the bench can confirm captured content. */
            while (UART_REG(UART_STATUS) & UART_STAT_TX_BUSY)
                ;
            UART_REG(UART_TX) = (uint32_t)b;
        }
    }

    uart_puts("uart rx PASS\r\n");
    while (1) { __asm__ volatile ("wfi"); }
    return 0;
}
