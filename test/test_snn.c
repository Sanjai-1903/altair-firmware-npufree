/*
 * test_snn.c — SNN accelerator smoke test
 * Configures SNN with static DRAM buffers, 4 LIF timesteps, polls DONE.
 * No stdlib / POSIX headers. No printf.
 *
 * Requires SNN hardware or RTL simulator.
 */

#include "snn.h"
#include "uart.h"

static uint32_t snn_weight_buf[SNN_MAX_NEURONS];

int main(void)
{
    int rc;

    uart_init(115200U);

    /* Step 1: reset and enable */
    snn_init();
    uart_puts("SNN init ok\r\n");

    /* Step 2: verify STATUS is not stuck BUSY after reset */
    if (SNN_REG(SNN_STATUS) & SNN_STAT_BUSY) {
        uart_puts("[FAIL] SNN busy after reset\r\n");
        return 1;
    }

    /* Step 3: configure LIF model, 4 timesteps (count of simulation steps)
     * Write order: WEIGHT_ADDR, THRESHOLD, LEAK, TIMESTEPS, NEURON_CONFIG
     * RTL has no SPIKE_INPUT_ADDR or OUTPUT_ADDR registers; input data
     * is written directly to synapse memory at offset 0x0C.             */
    snn_configure((uint32_t)snn_weight_buf,
                  0x00010000UL,   /* threshold = 1.0  (Q16 fixed-point) */
                  0x0000E666UL,   /* leak      ≈ 0.9  (Q16 fixed-point) */
                  4U,             /* 4 simulation timesteps (not ms)    */
                  SNN_NEURON_LIF);

    /* Step 4: launch (writes CONTROL = START, bit 0) */
    snn_launch();

    /* Step 5: poll STATUS bit 1 (DONE) */
    rc = snn_poll_done();

    if (rc == 0) {
        uart_puts("[PASS] SNN smoke test\r\n");
    } else {
        uart_puts("[FAIL] SNN error (STATUS.ERR set)\r\n");
    }

    snn_irq_clear();
    return rc;
}
