/*
 * dma.c — DMA channel driver
 * No stdlib / POSIX headers.
 * STATUS bit 0 = BUSY (1 = running)
 * STATUS bit 1 = DONE (1 = transfer complete)
 * STATUS bit 2 = ERR  (1 = bus / address error)
 * CONTROL bit 0 = START (write 1 to launch)
 */

#include "dma.h"

/* ISR status flags defined in trap.c; written only from M-mode trap context */
extern volatile uint32_t _dma_irq_done;
extern volatile uint32_t _dma_irq_err;

/* ── Public API ──────────────────────────────────────────────────────────── */

void dma_init(void)
{
    uint32_t ch;
    for (ch = 0U; ch < DMA_NUM_CHANNELS; ch++) {
        /* Abort any in-progress transfer first */
        DMA_REG(ch, DMA_CONTROL)   = 0U;
        DMA_REG(ch, DMA_SRC_ADDR)  = 0U;
        DMA_REG(ch, DMA_DST_ADDR)  = 0U;
        DMA_REG(ch, DMA_LENGTH)    = 0U;
    }
}

int dma_transfer(uint32_t ch, uint32_t src, uint32_t dst,
                 uint32_t len, uint32_t flags)
{
    if (ch >= DMA_NUM_CHANNELS)
        return -1;

    /* Write in this exact order: SRC, DST, LENGTH, then CONTROL last */
    DMA_REG(ch, DMA_SRC_ADDR) = src;
    DMA_REG(ch, DMA_DST_ADDR) = dst;
    DMA_REG(ch, DMA_LENGTH)   = len;
    /* Merge caller flags with START bit; write CONTROL last to launch */
    DMA_REG(ch, DMA_CONTROL)  = flags | DMA_CTRL_START;
    return 0;
}

int dma_poll_done(uint32_t ch)
{
    uint32_t st;
    uint32_t timeout = 0x100000UL;   /* ~2M cycles @ 50 MHz ≈ 40 ms */

    if (ch >= DMA_NUM_CHANNELS)
        return -1;

    do {
        st = DMA_REG(ch, DMA_STATUS);
        if (--timeout == 0U)
            return -1;               /* timeout — hardware never responded */
    } while (!(st & DMA_STAT_DONE));
    /* DONE set — transfer complete; check for error */
    return (st & DMA_STAT_ERR) ? -1 : 0;
}

/*
 * dma_irq_enable() — MUST be called before dma_transfer(), never during.
 * The |= read-modify-write on CONTROL will re-assert START=bit0 if a
 * transfer is already in flight, causing a double-launch on the channel.
 * Safe usage: set IRQ_EN flag in the flags argument of dma_transfer()
 * instead — e.g. dma_transfer(ch, src, dst, len, DMA_CTRL_IRQ_EN).
 */
void dma_irq_enable(uint32_t ch)
{
    if (ch >= DMA_NUM_CHANNELS)
        return;
    DMA_REG(ch, DMA_CONTROL) |= DMA_CTRL_IRQ_EN;
}

uint32_t dma_status(uint32_t ch)
{
    if (ch >= DMA_NUM_CHANNELS)
        return 0xFFFFFFFFUL;
    return DMA_REG(ch, DMA_STATUS);
}

void dma_abort(uint32_t ch)
{
    if (ch >= DMA_NUM_CHANNELS)
        return;
    DMA_REG(ch, DMA_CONTROL) = 0U;
}

int dma_irq_wait(uint32_t ch)
{
    uint32_t timeout = 0x100000UL;   /* matches dma_poll_done() — ~40 ms */
    uint32_t bit;

    if (ch >= DMA_NUM_CHANNELS)
        return -1;

    bit = 1U << ch;
    while (!(_dma_irq_done & bit) && !(_dma_irq_err & bit)) {
        if (--timeout == 0U)
            return -1;
    }
    return (_dma_irq_err & bit) ? -1 : 0;
}

void dma_irq_clear(uint32_t ch)
{
    uint32_t bit;
    if (ch >= DMA_NUM_CHANNELS)
        return;
    bit = 1U << ch;
    /* Called before transfer starts; no concurrent ISR access possible */
    _dma_irq_done &= ~bit;
    _dma_irq_err  &= ~bit;
}
