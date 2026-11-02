/*
 * test_dma.c — DMA memory-to-memory transfer smoke test
 * Uses CH3 (DMA_CH_PERIPH) as a plain mem-to-mem channel.
 * Source and destination are static arrays — linker places them in .bss/.data
 * automatically, giving valid DRAM addresses without hard-coded constants.
 * No stdlib / POSIX headers. No printf.
 */

#include "dma.h"
#include "uart.h"
#include "soc.h"

#define TEST_WORDS      16U
#define TEST_PATTERN    0xDEADBEEFUL

/* Static arrays → linker places these in DRAM (.bss) */
static uint32_t src_buf[TEST_WORDS];
static uint32_t dst_buf[TEST_WORDS];

int main(void)
{
    uint32_t i, fail;

    uart_init(115200U);
    dma_init();

    /* Fill source with incrementing pattern */
    for (i = 0U; i < TEST_WORDS; i++)
        src_buf[i] = TEST_PATTERN + i;

    /* Clear destination */
    for (i = 0U; i < TEST_WORDS; i++)
        dst_buf[i] = 0U;

    /* Launch CH3 mem→mem transfer (INC_SRC | INC_DST so addresses step) */
    if (dma_transfer(DMA_CH_PERIPH,
                     (uint32_t)src_buf,
                     (uint32_t)dst_buf,
                     TEST_WORDS * 4U,
                     DMA_CTRL_INC_SRC | DMA_CTRL_INC_DST) != 0) {
        uart_puts("[FAIL] DMA: invalid channel\r\n");
        return 1;
    }

    if (dma_poll_done(DMA_CH_PERIPH) != 0) {
        uart_puts("[FAIL] DMA: bus error\r\n");
        return 1;
    }

    /* Verify destination matches source pattern */
    fail = 0U;
    for (i = 0U; i < TEST_WORDS; i++) {
        if (dst_buf[i] != (TEST_PATTERN + i))
            fail++;
    }

    if (fail == 0U) {
        uart_puts("[PASS] DMA transfer\r\n");
        return 0;
    }

    uart_puts("[FAIL] DMA: ");
    uart_put_uint(fail);
    uart_puts(" word(s) mismatch\r\n");
    return (int)fail;
}
