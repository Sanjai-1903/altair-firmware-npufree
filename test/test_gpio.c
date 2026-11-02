/*
 * test_gpio.c — GPIO driver smoke test
 * Links as a standalone ELF with its own main().
 * No stdlib, no printf.
 */

#include "uart.h"
#include "gpio.h"

int main(void)
{
    uint32_t i;
    uint8_t  data;

    /* 1. UART first */
    uart_init(115200U);
    uart_puts("gpio test start\r\n");

    /* 2. GPIO init — all inputs, interrupts cleared */
    gpio_init();

    /* 3. Set heartbeat and anomaly pins as outputs */
    gpio_set_output(GPIO_PIN_HEARTBEAT | GPIO_PIN_ANOMALY);

    /* 4. Toggle heartbeat 4 times with a short delay between each.
     * Delay sized for RTL simulation (~40k cycles < 1 ms @ 50 MHz):
     * long enough for a testbench to observe each latch state, short
     * enough to keep the full image inside a cocotb test's time budget. */
    for (i = 0U; i < 4U; i++) {
        gpio_toggle(GPIO_PIN_HEARTBEAT);
        volatile uint32_t d = 10000U;
        while (d--) { /* nothing */ }
    }

    /* 5. Set anomaly pin high then low */
    gpio_set(GPIO_PIN_ANOMALY);
    {
        volatile uint32_t d = 5000U;
        while (d--) { /* nothing */ }
    }
    gpio_clear(GPIO_PIN_ANOMALY);

    /* 6. Read GPIO_DATA back and print as hex */
    data = gpio_read();
    uart_puts("gpio data: ");
    uart_put_hex32((uint32_t)data);
    uart_puts("\r\n");

    /* 7. Interrupt enable/status: leave DEBUG0 as an input with
     * mismatch-interrupt enabled, then report GPIO_INTERRUPT_STATUS.
     * A cocotb testbench can drive the pin externally (COCOTB_SIM's
     * gpio_test_in hook — see gpio_top.sv) during this window to
     * exercise the mismatch-detection and irq-pulse RTL path; run
     * standalone (no external drive), the pin stays at its synchronizer
     * reset value and status reads back 0. */
    gpio_set_input(GPIO_PIN_DEBUG0);
    gpio_irq_enable(GPIO_PIN_DEBUG0);
    {
        volatile uint32_t d = 10000U;
        while (d--) { /* let an external drive (if any) settle through the
                          2-flop synchronizer before the status read */ }
    }
    uart_puts("gpio irq status: ");
    uart_put_hex32((uint32_t)gpio_irq_status());
    uart_puts("\r\n");

    /* 8. Input-register readback while the external drive (if any) is
     * still holding DEBUG0 high — verifies GPIO_DATA reads return the
     * synchronized pin state (the RTL's documented behaviour; there is
     * no output-latch readback). */
    data = gpio_read();
    uart_puts("gpio data2: ");
    uart_put_hex32((uint32_t)data);
    uart_puts("\r\n");

    uart_puts("gpio PASS\r\n");
    return 0;
}
