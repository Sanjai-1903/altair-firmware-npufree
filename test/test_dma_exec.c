/*
 * test_dma_exec.c — DMA execution verification, polled mode (Phase 8, Part B)
 *
 * Complements test_dma.c (basic mem-to-mem smoke test) and test_dma_irq.c
 * (interrupt-mode completion/timeout/recovery) with the remaining checklist
 * items: BUSY/DONE register-level observation, back-to-back transfers on
 * one channel, two channels started close together (arbitration), a
 * zero-length transfer, and an invalid destination address.
 *
 * RTL contract exercised (dma_fsm.sv, dma_regs.sv):
 *   - STATUS bit0=BUSY, bit1=DONE, bit2=ERR (read-only; DONE/ERR persist
 *     until the channel's NEXT start, not cleared by any write — see
 *     dma_fsm.sv IDLE state: done_lat_d[i]/err_lat_d[i] reset only when
 *     ch_start_i[i] fires for that channel).
 *   - A zero-length transfer (LENGTH=0) skips the bus entirely and goes
 *     straight to DONE — no read or write is ever issued.
 *   - The FSM services one channel at a time in fixed priority order
 *     (channel 0 highest); starting two channels back-to-back is legal —
 *     both start requests latch and are serviced sequentially.
 *   - An unmapped destination address (outside every slave's decoded
 *     range) hits addr_decoder's default_sel_o path, which the bus_mux
 *     acks immediately with wb_err_o=1 — dma_fsm latches this as
 *     STATUS.ERR without needing its own 1024-cycle ACK_TIMEOUT_CYCLES
 *     internal timeout.
 *
 * No stdlib / POSIX headers.
 */

#include "dma.h"
#include "uart.h"
#include "soc.h"

#define TEST_WORDS      8U
#define TEST_PATTERN    0x11223344UL

/* Unmapped: beyond every slave's decoded range in addr_map_pkg (highest
 * is DEBUG_BASE=0x50000000 + a small window) — lands on addr_decoder's
 * default_sel_o / bus_mux's immediate wb_err_o=1 path. */
#define INVALID_DEST    0x60000000UL

static uint32_t src_buf[TEST_WORDS];
static uint32_t dst_buf[TEST_WORDS];
static uint32_t dst_buf2[TEST_WORDS];
static uint32_t ch0_src_buf[TEST_WORDS];
static uint32_t ch0_dst_buf[TEST_WORDS];

static uint32_t fill_check(uint32_t *dst)
{
    uint32_t i, fail = 0U;
    for (i = 0U; i < TEST_WORDS; i++)
        if (dst[i] != (TEST_PATTERN + i)) fail++;
    return fail;
}

int main(void)
{
    uint32_t i, fail, st;

    uart_init(115200U);
    dma_init();
    uart_puts("dma exec test start\r\n");

    for (i = 0U; i < TEST_WORDS; i++) {
        src_buf[i] = TEST_PATTERN + i;
        ch0_src_buf[i] = TEST_PATTERN + 0x1000U + i;
    }

    /* ── Test 1: BUSY during transfer, DONE after ─────────────────────────── */
    for (i = 0U; i < TEST_WORDS; i++) dst_buf[i] = 0U;

    dma_transfer(DMA_CH_PERIPH, (uint32_t)src_buf, (uint32_t)dst_buf,
                 TEST_WORDS * 4U, DMA_CTRL_INC_SRC | DMA_CTRL_INC_DST);
    st = dma_status(DMA_CH_PERIPH);   /* sampled as soon as CPU can read back */
    uart_puts("t1 status right after launch: ");
    uart_put_hex32(st);
    uart_puts("\r\n");

    if (dma_poll_done(DMA_CH_PERIPH) != 0) {
        uart_puts("[FAIL] t1: unexpected bus error\r\n");
        return 1;
    }
    st = dma_status(DMA_CH_PERIPH);
    if (!(st & DMA_STAT_DONE)) {
        uart_puts("[FAIL] t1: STATUS.DONE not set after poll\r\n");
        return 1;
    }
    if (st & DMA_STAT_BUSY) {
        uart_puts("[FAIL] t1: STATUS.BUSY still set after DONE\r\n");
        return 1;
    }
    fail = fill_check(dst_buf);
    if (fail) {
        uart_puts("[FAIL] t1: data mismatch\r\n");
        return 1;
    }
    uart_puts("[PASS] t1: busy/done status + data correct\r\n");

    /* ── Test 2: back-to-back transfers, same channel ─────────────────────── */
    for (i = 0U; i < TEST_WORDS; i++) dst_buf[i] = 0U;
    dma_transfer(DMA_CH_PERIPH, (uint32_t)src_buf, (uint32_t)dst_buf,
                 TEST_WORDS * 4U, DMA_CTRL_INC_SRC | DMA_CTRL_INC_DST);
    if (dma_poll_done(DMA_CH_PERIPH) != 0) {
        uart_puts("[FAIL] t2a: bus error\r\n");
        return 1;
    }

    for (i = 0U; i < TEST_WORDS; i++) dst_buf2[i] = 0U;
    dma_transfer(DMA_CH_PERIPH, (uint32_t)src_buf, (uint32_t)dst_buf2,
                 TEST_WORDS * 4U, DMA_CTRL_INC_SRC | DMA_CTRL_INC_DST);
    if (dma_poll_done(DMA_CH_PERIPH) != 0) {
        uart_puts("[FAIL] t2b: bus error\r\n");
        return 1;
    }

    fail = fill_check(dst_buf) + fill_check(dst_buf2);
    if (fail) {
        uart_puts("[FAIL] t2: back-to-back data mismatch\r\n");
        return 1;
    }
    uart_puts("[PASS] t2: back-to-back transfers (same channel)\r\n");

    /* ── Test 3: two channels started close together (arbitration) ────────── */
    for (i = 0U; i < TEST_WORDS; i++) {
        ch0_dst_buf[i] = 0U;
        dst_buf[i] = 0U;
    }
    dma_transfer(DMA_CH_GP0, (uint32_t)ch0_src_buf, (uint32_t)ch0_dst_buf,
                 TEST_WORDS * 4U, DMA_CTRL_INC_SRC | DMA_CTRL_INC_DST);
    dma_transfer(DMA_CH_PERIPH, (uint32_t)src_buf, (uint32_t)dst_buf,
                 TEST_WORDS * 4U, DMA_CTRL_INC_SRC | DMA_CTRL_INC_DST);

    if (dma_poll_done(DMA_CH_GP0) != 0) {
        uart_puts("[FAIL] t3: ch0 bus error\r\n");
        return 1;
    }
    if (dma_poll_done(DMA_CH_PERIPH) != 0) {
        uart_puts("[FAIL] t3: ch3 bus error\r\n");
        return 1;
    }
    fail = 0U;
    for (i = 0U; i < TEST_WORDS; i++) {
        if (ch0_dst_buf[i] != (TEST_PATTERN + 0x1000U + i)) fail++;
        if (dst_buf[i] != (TEST_PATTERN + i)) fail++;
    }
    if (fail) {
        uart_puts("[FAIL] t3: multi-channel data mismatch\r\n");
        return 1;
    }
    uart_puts("[PASS] t3: two channels started back-to-back, both correct\r\n");

    /* ── Test 4: zero-length transfer ─────────────────────────────────────── */
    dst_buf[0] = 0xAAAAAAAAUL;   /* sentinel — must NOT be touched */
    dma_transfer(DMA_CH_PERIPH, (uint32_t)src_buf, (uint32_t)dst_buf, 0U, 0U);
    if (dma_poll_done(DMA_CH_PERIPH) != 0) {
        uart_puts("[FAIL] t4: zero-length transfer reported error\r\n");
        return 1;
    }
    st = dma_status(DMA_CH_PERIPH);
    if (!(st & DMA_STAT_DONE)) {
        uart_puts("[FAIL] t4: zero-length transfer never set DONE\r\n");
        return 1;
    }
    if (dst_buf[0] != 0xAAAAAAAAUL) {
        uart_puts("[FAIL] t4: zero-length transfer touched memory\r\n");
        return 1;
    }
    uart_puts("[PASS] t4: zero-length transfer completes with no bus activity\r\n");

    /* ── Test 5: invalid configuration (unmapped destination) ───────────────
     * dma_poll_done() only exits its loop on STATUS.DONE, but dma_fsm.sv's
     * bus-error path transitions straight to its ERR state without ever
     * setting DONE for this channel (only DONE's own state sets
     * done_lat_d) — so dma_poll_done() would always spin through its
     * entire ~40ms-nominal shared-driver timeout before returning -1.
     * Poll STATUS directly instead, watching BUSY clear (bounded: the
     * unmapped access resolves in a handful of cycles via bus_mux's
     * immediate default_sel_o error path, well under dma_fsm's own
     * 1024-cycle ACK_TIMEOUT_CYCLES fallback). */
    dma_transfer(DMA_CH_PERIPH, (uint32_t)src_buf, INVALID_DEST, 4U, 0U);
    {
        uint32_t timeout = 200000U;
        while ((dma_status(DMA_CH_PERIPH) & DMA_STAT_BUSY) && --timeout) { }
        if (timeout == 0U) {
            uart_puts("[FAIL] t5: unmapped-destination transfer never left BUSY\r\n");
            return 1;
        }
    }
    st = dma_status(DMA_CH_PERIPH);
    if (!(st & DMA_STAT_ERR)) {
        uart_puts("[FAIL] t5: STATUS.ERR not set for unmapped dest\r\n");
        return 1;
    }
    uart_puts("[PASS] t5: invalid configuration correctly reports STATUS.ERR\r\n");

    dma_init();   /* leave all channels clean */

    uart_puts("dma exec PASS\r\n");
    return 0;
}
