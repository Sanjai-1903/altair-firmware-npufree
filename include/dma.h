#ifndef DMA_H
#define DMA_H

/*
 * dma.h — DMA channel driver API
 * 4 channels, each with a 0x40-byte stride register block.
 * CH0: general-purpose  CH1: general-purpose  CH2: mem→SNN  CH3: periph↔mem
 *
 * NPU-FREE BUILD: CH0/CH1 were named mem→NPU and NPU→mem. The NPU block
 * they moved data to and from no longer exists, so their old destination
 * routing (dma_fsm.sv's dma_addr[15:12] decode onto NPU weight/activation
 * SRAM) is unreachable. The channels themselves are untouched RTL and
 * still work as general mem↔mem movers -- channel index is a software
 * convention, routing is by destination address (see below) -- so they are
 * kept and renamed rather than deleted.
 *
 * STATUS register (RTL): bit 0 = BUSY (1=running), bit 1 = DONE (1=complete),
 * bit 2 = ERR (1=bus error).  No circular/descriptor mode in RTL.
 * No stdlib / POSIX headers.
 */

#include <stdint.h>
#include "soc.h"

/* ─── DMA channel count and stride ──────────────────────────────────────── */
#define DMA_NUM_CHANNELS    4U
#define DMA_CHAN_STRIDE     0x40UL

/* ─── Per-channel register offsets (relative to channel base) ───────────── */
#define DMA_SRC_ADDR        0x00U
#define DMA_DST_ADDR        0x04U
#define DMA_LENGTH          0x08U
#define DMA_CONTROL         0x0CU
#define DMA_STATUS          0x10U
/* NOT IMPLEMENTED IN RTL — writes are ignored */
#define DMA_NEXT_DESC       0x14U       /* (reserved)                        */

/* ─── Channel indices ────────────────────────────────────────────────────── */
/* NOTE: channel index is a software convention only (dma_fsm.sv routes by
 * destination *address*, not by channel number — see dma.c/dma_fsm.sv).
 * Any channel can target any address; the names below just document intent. */
#define DMA_CH_GP0          0U   /* general-purpose (formerly mem→NPU) */
#define DMA_CH_GP1          1U   /* general-purpose (formerly NPU→mem) */
/*
 * DMA_CH_MEM_TO_SNN — FIXED in Phase 1, not yet switched to by production
 * firmware. RTL routes this to synapse_mem (dma_fsm.sv's is_snn +
 * snn_top.sv's dma_req_i priority mux). Previously snn_top.sv discarded
 * the DMA byte-enable (dma_be_i) and synapse_mem.sv had no byte-enable
 * input at all, so any word/burst-granularity transfer silently dropped
 * 3 of every 4 synapse bytes -- separately, the synapse address window
 * also collided with the control-register range (see snn.h). Both root
 * causes are now fixed: synapse_mem.sv has a byte-enabled 32-bit write
 * port (mirrors weight_sram.sv), snn_top.sv passes dma_be_i/dma_wdata_i
 * through unmodified, and synapse memory has its own address window
 * (SNN_SYNAPSE_WINDOW in snn.h) separate from the registers. Bulk-load
 * synapse preload via this channel should now be usable, pending
 * confirmation via a test-snn/test-dma cocotb regression run. Firmware
 * still uses snn_write_synapse()'s direct WB byte-write path today; that
 * function is unaffected by this fix (still correct after it, just not
 * the bulk-transfer path).
 */
#define DMA_CH_MEM_TO_SNN   2U
#define DMA_CH_PERIPH       3U

/* Canonical aliases required by audit spec */
#define DMA_CH_SNN_TX       2U   /* mem → SNN  */
#define DMA_CH_PERIPH_IO    3U   /* periph ↔ mem */

/* ─── DMA CONTROL bit fields ─────────────────────────────────────────────── */
#define DMA_CTRL_START      (1U << 0)   /* bit 0: write 1 to launch transfer */
#define DMA_CTRL_IRQ_EN     (1U << 1)   /* bit 1: enable done interrupt      */
/* NOT IMPLEMENTED IN RTL — writes are ignored */
#define DMA_CTRL_CIRC       (1U << 2)   /* bit 2: circular/descriptor mode   */
/* NOT IMPLEMENTED IN RTL — writes are ignored */
#define DMA_CTRL_INC_SRC    (1U << 3)   /* bit 3: auto-increment source addr */
/* NOT IMPLEMENTED IN RTL — writes are ignored */
#define DMA_CTRL_INC_DST    (1U << 4)   /* bit 4: auto-increment dest addr   */

/* ─── DMA STATUS bit fields (matches RTL dma_regs.sv) ────────────────────── */
#define DMA_STAT_BUSY       (1U << 0)   /* bit 0: 1 = transfer in progress    */
#define DMA_STAT_DONE       (1U << 1)   /* bit 1: 1 = transfer complete       */
#define DMA_STAT_ERR        (1U << 2)   /* bit 2: 1 = bus / address error     */

/* ─── Channel base address helper ───────────────────────────────────────── */
/* Spec name: DMA_CH_BASE(n) */
#define DMA_CH_BASE(n)      (DMA_BASE + (uint32_t)(n) * DMA_CHAN_STRIDE)
#define DMA_REG(ch, off)    REG(DMA_CH_BASE(ch), off)

/* ─── Driver API ─────────────────────────────────────────────────────────── */

/** dma_init() - Clear and disable all DMA channels. */
void dma_init(void);

/**
 * dma_transfer() - Configure and launch a single DMA transfer.
 * Writes SRC_ADDR, DST_ADDR, LENGTH, then CONTROL (with START bit set).
 * @flags: optional DMA_CTRL_* ORed together (START is always added).
 * Returns 0 on success, -1 if ch >= DMA_NUM_CHANNELS.
 */
int dma_transfer(uint32_t ch,
                 uint32_t src,
                 uint32_t dst,
                 uint32_t len,
                 uint32_t flags);

/**
 * dma_poll_done() - Blocking spin until STATUS.DONE (bit 1) is set.
 * Returns 0 on success, -1 on error (STATUS.ERR bit 2) or invalid channel.
 */
int dma_poll_done(uint32_t ch);

/** dma_irq_enable() - Set IRQ_EN bit for a channel. */
void dma_irq_enable(uint32_t ch);

/** dma_status() - Return raw STATUS register; 0xFFFFFFFF if invalid ch. */
uint32_t dma_status(uint32_t ch);

/** dma_abort() - Write CONTROL=0 to halt a transfer immediately. */
void dma_abort(uint32_t ch);

/**
 * dma_irq_wait() - Spin-wait for interrupt-driven DMA completion.
 * Polls volatile ISR flags set by machine_trap_handler in trap.c.
 * Requires: DMA_CTRL_IRQ_EN passed to dma_transfer() AND
 *           intc_enable(IRQ_DMA) called before the transfer.
 * Call dma_irq_clear(ch) before each new transfer to reset the flags.
 * Returns 0 on completion, -1 on error (STATUS.ERR bit 2) or timeout (~40 ms).
 */
int dma_irq_wait(uint32_t ch);

/**
 * dma_irq_clear() - Reset ISR done/error flags for a channel.
 * Must be called before starting each interrupt-driven transfer to prevent
 * a stale flag from a prior transfer causing dma_irq_wait() to return early.
 */
void dma_irq_clear(uint32_t ch);

#endif /* DMA_H */
