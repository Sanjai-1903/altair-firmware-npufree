/*
 * main.c — top-level inference loop
 * ALTair firmware. Ibex RV32IM, 50 MHz, bare-metal, M-mode.
 * No stdlib / POSIX headers. No printf. No global mutable state.
 *
 * NPU-FREE BUILD. There is no npu_init() step and no NPU interrupt to
 * arm; the SNN is the only accelerator brought up here. The Conv1D layers
 * run on the CPU inside inference_run() -- see conv1d.h.
 *
 * Init order: uart -> intc -> timer -> dma -> gpio -> spi -> i2c -> pwm ->
 *             snn -> inference -> watchdog
 */

#include "soc.h"
#include "uart.h"
#include "intc.h"
#include "timer.h"
#include "dma.h"
#include "snn.h"
#include "gpio.h"
#include "spi.h"
#include "i2c.h"
#include "pwm.h"
#include "telemetry.h"
#include "inference.h"
#include "latency.h"

/* dbg_trap_dump_pending() (trap.c) — mainline-context printer for the trap
 * ring buffer captured by crt0.S. Kept: it belongs to the separate
 * illegal-instruction investigation and is independent of the NPU. */
extern void dbg_trap_dump_pending(void);

/* stack_watermark_check() - Scan stack for 0xDEADBEEF pattern.
 * Returns number of bytes of stack still showing watermark
 * (= unused stack space). Print via UART for profiling.         */
static void stack_watermark_check(void)
{
    extern uint32_t _stack_bottom;
    extern uint32_t _stack_top;
    volatile uint32_t *p = &_stack_bottom;
    uint32_t free_words = 0U;

    while (p < &_stack_top && *p == 0xDEADBEEFUL) {
        free_words++;
        p++;
    }
    uart_puts("stack free: ");
    uart_put_uint(free_words * 4U);
    uart_puts(" bytes\r\n");
}

/* ── Entry point ─────────────────────────────────────────────────────────── */
int main(void)
{
    /* 1. UART first — all other inits can report over serial */
    uart_init(115200U);
    uart_puts("boot ok\r\n");

    /* 2. Interrupt controller */
    intc_init();
    uart_puts("intc ok\r\n");

    /* 3. Timer — prescaler=1 (no division, raw 50 MHz ticks) */
    timer_init(0U);
    uart_puts("timer ok\r\n");

    /* 4. DMA — reset all channels. Still brought up: CH2 is the mem->SNN
     * path and CH3 is periph<->mem. CH0/CH1 are general-purpose now that
     * the NPU they used to target is gone (see dma.h). */
    dma_init();
    uart_puts("dma ok\r\n");

    /* 5. GPIO */
    gpio_init();
    gpio_set_output(GPIO_PIN_HEARTBEAT);
    uart_puts("gpio ok\r\n");

    /* 6. SPI — CLKDIV=4: SCLK = 50 MHz / 10 (matches verified test_spi.c) */
    spi_init(4U);
    uart_puts("spi ok\r\n");

    /* 7. I2C — CLKDIV=9: SCL = 50 MHz / 20 (matches verified test_i2c.c) */
    i2c_init(9U);
    uart_puts("i2c ok\r\n");

    /* 8. PWM — resets to disabled output, period=0xFFFF, all duties=0 */
    pwm_init();
    uart_puts("pwm ok\r\n");

    /* 9. SNN — the only accelerator in this build. Zeroes the synapse and
     * input-buffer windows, which have no hardware reset. */
    snn_init();
    uart_puts("snn ok\r\n");

    /* 10. Inference engine — arms the sensor UART RX path. No weight
     * preload step: the Conv1D tiles live in .rodata (see conv1d.c). */
    inference_init();
    uart_puts("inference ok\r\n");

    /* 11. Watchdog — 5-second timeout; kick every inference loop iteration */
    watchdog_init(5000U);
    uart_puts("watchdog ok\r\n");

    /* Stack watermark report — call before enabling interrupts           */
    stack_watermark_check();

    /* Enable global interrupts now that all peripherals are configured */
    intc_global_enable();
    uart_puts("ready\r\n");
    telemetry_init();

    /* ── Main inference loop ──────────────────────────────────────────── */
    /* Each iteration blocks on sensor_read_window() (real UART sensor
     * acquisition — see sensor.c) inside inference_run(); there is no
     * synthetic or hardcoded input buffer here. */
    while (1) {
        /* Print any newly-captured trap ring-buffer entries from mainline
         * context (not from the ISR — a trap must not hold off interrupt
         * servicing for the ~6 ms an entry costs on this FIFO-less UART). */
        dbg_trap_dump_pending();

        int result = inference_run();
        if (result == INFERENCE_ERROR) {
            /* Payload = g_inference_error_stage: 1 = sensor window
             * timeout, 6 = SNN never asserted DONE. Codes 2..5 (L1/L2 DMA
             * and NPU) are retired with the NPU block — see inference.h. */
            telemetry_send_error((uint8_t)g_inference_error_stage);
        } else {
            uint32_t last_us, avg_us, min_us, max_us;
            telemetry_send_inference((uint8_t)result);
            latency_get_stats(&last_us, &avg_us, &min_us, &max_us);
            telemetry_send_latency(last_us, avg_us, min_us, max_us);
        }
        watchdog_kick();
        gpio_toggle(GPIO_PIN_HEARTBEAT);
    }

    /* Unreachable — crt0 hangs if main returns */
    return 0;
}
