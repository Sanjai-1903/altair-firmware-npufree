#ifndef SNN_H
#define SNN_H

/*
 * snn.h — Spiking Neural Network accelerator driver API
 * No stdlib / POSIX headers.
 *
 * Verified RTL contract (snn_regs.sv, snn_ctrl.sv, snn_top.sv, neuron_core.sv,
 * input_buffer.sv) — updated for the Phase 1 SNN address-window / byte-enable
 * fix and the subsequent input-buffer addition (see below; both require
 * re-verification via a test-snn/test-dma regression run before being
 * treated as simulation-proven):
 *
 * Address decode is now 3-way on wb_addr_i[9:8]: 00 = register file
 * (this comment block), 01 = synapse window (Phase 1), 10 = input buffer
 * (see below). 11 is unused/reserved.
 *
 *   SNN_CONTROL  [0x00]
 *     bit 0 = START — drives snn_ctrl.start; FSM begins timestep loop
 *     bit 1 = RESET — RTL NOT IMPLEMENTED (stored in control_reg but never
 *                     read by snn_ctrl or any other module; writing it has
 *                     no hardware effect on the FSM or datapath)
 *
 *   SNN_STATUS   [0x04]  read-only
 *     bit 0 = BUSY — driven by snn_ctrl; 1 during S_EXEC
 *     bit 1 = DONE — sticky done latch, held until the next START
 *     bit 2 = INIT_DONE — all SNN init-tracked memories have been written
 *
 *   Synapse memory window  [SNN_BASE + 0x100 .. SNN_BASE + 0x1FF]
 *     256 individually byte-addressable entries, one per neuron
 *     (offset 0x100 + neuron_idx = synapse_mem[neuron_idx]).
 *     FIXED in Phase 1: this window previously collided with the control/
 *     status/config register range (0x00-0x4C) — the synapse "port" was
 *     only reachable at the single coincidental offset 0x0C (neuron 12);
 *     every other neuron_idx silently hit a control register or was
 *     dropped. snn_top.sv now decodes wb_addr_i[8] to route the synapse
 *     window to a dedicated, byte-enabled 32-bit write port on
 *     synapse_mem (mirrors weight_sram.sv's be[3:0] pattern), separate
 *     from snn_regs.sv's register file. DMA CH2 (mem→SNN) now correctly
 *     honors byte-enables/full words instead of only landing byte lane 0,
 *     so burst DMA transfers into synapse memory are byte-correct.
 *     Use snn_write_synapse() — do NOT write a DRAM pointer here.
 *
 *   Output spike buffer  [0x50, 0x54, ..., 0x6C]  read-only
 *     ADDED in Phase 1: 8 x 32-bit words = the live 256-bit spike_vector
 *     from the neuron array (spike_vector[31:0] at 0x50, ...,
 *     spike_vector[255:224] at 0x6C). Previously the SNN's output was
 *     computed in RTL but never exposed to the CPU through any register —
 *     this was a hard blocker for using the SNN's result in a decision.
 *     Mirrors the CONNECT_MASK word layout. See SNN_SPIKE_OUT_* below.
 *
 *   SNN_THRESHOLD  [0x14]  R/W  (16-bit; reset = 20)
 *     Membrane threshold for LIF firing; consumed by neuron_core.
 *
 *   SNN_LEAK       [0x18]  R/W  (16-bit; reset = 1)
 *     Per-cycle membrane leak; consumed by neuron_core every timestep.
 *
 *   SNN_TIMESTEPS  [0x1C]  R/W  (16-bit; reset = 10)
 *     Number of simulation timesteps; consumed by timestep_counter in
 *     snn_top.sv; execution completes when counter >= timesteps.
 *
 *   SNN_NEURON_CONFIG / CONNECT_MASK_0  [0x20]  R/W
 *     NOT a neuron model selector.  This is bits[31:0] of the 256-bit
 *     connect_mask used by spike_router.  Writing SNN_NEURON_LIF (0x00)
 *     sets the first 32 connectivity bits to zero (disconnects all neurons
 *     in that slice); writing SNN_NEURON_IF (0x01) sets only bit 0.
 *     SNN_NEURON_LIF and SNN_NEURON_IF codes are RTL NOT IMPLEMENTED as
 *     neuron model selectors — the hardware uses a single fixed LIF model.
 *
 *   CONNECT_MASK words [0x30..0x4C]  R/W
 *     Full 256-bit connect_mask, written in 8 × 32-bit words.
 *     Offset 0x30 = bits[31:0], 0x34 = bits[63:32], ..., 0x4C = bits[255:224].
 *
 *   SNN_INTERRUPT_CLR  [0x28]  W
 *     Any write sets irq_clr for one cycle, clearing the irq_latched
 *     flip-flop in snn_top.sv.  The bit value written does not matter.
 *     No read path — reads return 0.
 *
 * IRQ path:
 *   snn_top.irq_latched latches when done=1.
 *   irq_o = irq_latched (no enable register — always armed).
 *   irq_clr (from any write to 0x28) resets irq_latched to 0.
 *   SNN has NO local interrupt-enable register; arm via intc_enable(IRQ_SNN).
 *
 * Soft reset:
 *   RTL NOT IMPLEMENTED — CONTROL[1] is stored but not wired.
 *   The FSM resets via rst_n (hardware reset) only.
 *   snn_soft_reset() below waits for BUSY=0 and clears CONTROL; it does
 *   NOT assert any hardware reset.
 *
 *   Input buffer window  [SNN_BASE + 0x200 .. SNN_BASE + 0x2FF]
 *     ADDED: 256 individually byte-addressable entries, one per neuron
 *     (offset 0x200 + neuron_idx = input_buffer[neuron_idx]), same
 *     byte-enabled write technique as the synapse window. Implements
 *     Architecture_spec.pdf sec. 10's "spike input buffer" -- the one
 *     named SNN module that had never been built (see architecture
 *     restoration report). Holds a signed INT8 external stimulus value per
 *     neuron, added into that neuron's membrane update every cycle
 *     (neuron_core.sv's ext_current_i), independent of and in addition to
 *     the existing spike_in/synapse-weight self-feedback path. Values are
 *     feature-domain currents, not pre-encoded spikes -- no hardware spike
 *     encoder exists or is planned. Defaults to 0 (no effect on membrane,
 *     identical to pre-input-buffer behavior) once snn_init() has zeroed
 *     it -- see snn_init()'s comment for why that zero-fill is required on
 *     real hardware even though simulation would silently read 0 without
 *     it. Use snn_write_input() below; do NOT use SNN_REG() (fixed offset,
 *     not neuron_idx).
 *
 * Dead RTL (defined but not instantiated in snn_top.sv):
 *   spike_mem.sv  — NOT instantiated
 *   synapse_core.sv — NOT instantiated
 *
 * Neuron model:
 *   One fixed LIF (Leaky Integrate-and-Fire) model is implemented.
 *   SNN_NEURON_IF and SNN_NEURON_LIF are RTL NOT IMPLEMENTED as model
 *   selectors — the neuron_core always performs LIF regardless of
 *   what is written to offset 0x20.
 *
 * Connect mask default:
 *   On reset, connect_mask resets to all-ones (all 256 bits = 1),
 *   meaning all neurons are connected by default.
 */

#include <stdint.h>
#include "soc.h"
#include "intc.h"

/* ─── SNN register offsets (from snn_pkg.sv) ────────────────────────────── */
#define SNN_CONTROL          0x00U   /* R/W — bit0=START (functional) */
#define SNN_STATUS           0x04U   /* R   — bit0=BUSY, bit1=DONE,
                                        bit2=INIT_DONE */
#define SNN_THRESHOLD        0x14U   /* R/W — 16-bit membrane threshold */
#define SNN_LEAK             0x18U   /* R/W — 16-bit leak per timestep */
#define SNN_TIMESTEPS        0x1CU   /* R/W — 16-bit timestep count */
#define SNN_CONNECT_MASK_0   0x20U   /* R/W — connect_mask bits[31:0]
                                        (formerly mislabeled NEURON_CONFIG) */
#define SNN_INTERRUPT_CLR    0x28U   /* W   — any write clears IRQ latch */
#define SNN_CONNECT_MASK_1   0x30U   /* R/W — connect_mask bits[63:32]  */
#define SNN_CONNECT_MASK_2   0x34U   /* R/W — connect_mask bits[95:64]  */
#define SNN_CONNECT_MASK_3   0x38U   /* R/W — connect_mask bits[127:96] */
#define SNN_CONNECT_MASK_4   0x3CU   /* R/W — connect_mask bits[159:128]*/
#define SNN_CONNECT_MASK_5   0x40U   /* R/W — connect_mask bits[191:160]*/
#define SNN_CONNECT_MASK_6   0x44U   /* R/W — connect_mask bits[223:192]*/
#define SNN_CONNECT_MASK_7   0x48U   /* R/W — connect_mask bits[255:224]*/

/* ─── Output spike buffer (ADDED in Phase 1) — read-only ─────────────────── */
#define SNN_SPIKE_OUT_0      0x50U   /* R   — spike_vector bits[31:0]   */
#define SNN_SPIKE_OUT_1      0x54U   /* R   — spike_vector bits[63:32]  */
#define SNN_SPIKE_OUT_2      0x58U   /* R   — spike_vector bits[95:64]  */
#define SNN_SPIKE_OUT_3      0x5CU   /* R   — spike_vector bits[127:96] */
#define SNN_SPIKE_OUT_4      0x60U   /* R   — spike_vector bits[159:128]*/
#define SNN_SPIKE_OUT_5      0x64U   /* R   — spike_vector bits[191:160]*/
#define SNN_SPIKE_OUT_6      0x68U   /* R   — spike_vector bits[223:192]*/
#define SNN_SPIKE_OUT_7      0x6CU   /* R   — spike_vector bits[255:224]*/

/* ─── Synapse memory window (FIXED in Phase 1) ────────────────────────────
 * 256 byte-addressable entries at SNN_BASE + SNN_SYNAPSE_WINDOW + neuron_idx
 * (neuron_idx = 0..255). Decoded in snn_top.sv via wb_addr_i[8]; entirely
 * separate from the SNN_CONTROL..SNN_SPIKE_OUT_7 register range above, so
 * there is no address collision. Do not use SNN_REG() for this — it adds a
 * fixed offset, not neuron_idx. Use snn_write_synapse() below, which
 * computes the correct absolute address.
 */
#define SNN_SYNAPSE_WINDOW   0x100U

/* ─── Input buffer window (ADDED — spike input buffer, Architecture_spec.pdf
 * sec. 10) ──────────────────────────────────────────────────────────────
 * 256 byte-addressable entries at SNN_BASE + SNN_INPUT_BUFFER_WINDOW +
 * neuron_idx. Decoded in snn_top.sv via wb_addr_i[9:8]==2'b10; separate
 * from both the register range and the synapse window, so no address
 * collision with either. Holds signed INT8 external stimulus (feature)
 * values, not spikes -- see neuron_core.sv's ext_current_i. No driver
 * function exists yet; this constant only documents the RTL address map.
 */
#define SNN_INPUT_BUFFER_WINDOW   0x200U

/* Number of neurons the CNN->SNN pipeline actually drives (matches the
 * Conv1D front-end's GlobalAveragePooling1D output width, one feature per
 * input neuron). The other 240 neurons' input_buffer entries stay 0
 * (see snn_init()) and behave exactly as before this change. */
#define SNN_NUM_INPUT_NEURONS     16U

/*
 * SNN_SYNAPSE_PORT / SNN_WEIGHT_ADDR (formerly 0x0CU) removed — the RTL
 * they described no longer exists as of the Phase 1 fix (synapse memory
 * moved to its own address window; see SNN_SYNAPSE_WINDOW above). Verified
 * unused by any test or firmware call site before removal (grep across
 * firmware/ found no references outside this header and snn.c).
 */

/*
 * Legacy alias — kept for backward-compatibility with test callsites that
 * pass a neuron_cfg value.  In RTL, offset 0x20 is connect_mask[31:0].
 * Writing 0x00 (SNN_NEURON_LIF) zeroes the first 32 connectivity bits.
 * Writing 0x01 (SNN_NEURON_IF) sets only connectivity bit 0.
 * This alias is intentionally retained; the connect_mask effect is documented.
 */
#define SNN_NEURON_CONFIG    SNN_CONNECT_MASK_0

/* ─── SNN CONTROL bit fields ─────────────────────────────────────────────── */
#define SNN_CTRL_START      (1U << 0)   /* bit 0: start timestep simulation */
/*
 * SNN_CTRL_RESET (bit 1) is RTL NOT IMPLEMENTED.
 * Writing this bit to CONTROL has no hardware effect.
 * Retained for documentation only.
 */
#define SNN_CTRL_RESET      (1U << 1)   /* RTL NOT IMPLEMENTED */

/* ─── SNN STATUS bit fields ──────────────────────────────────────────────── */
#define SNN_STAT_BUSY       (1U << 0)   /* bit 0: simulation running */
#define SNN_STAT_DONE       (1U << 1)   /* bit 1: all timesteps complete */
#define SNN_STAT_INIT_DONE  (1U << 2)   /* bit 2: memory init guard satisfied */
#define SNN_STAT_ERR        0U          /* no SNN ERR status bit in current RTL */

/* ─── SNN IRQ constants ──────────────────────────────────────────────────── */
/*
 * SNN has NO interrupt-enable register.  The IRQ latch is always armed.
 * Any write to SNN_INTERRUPT_CLR clears irq_latched regardless of bit value.
 * The constants below are retained for documentation and clearing calls.
 */
#define SNN_INT_DONE        (1U << 1)   /* use to write SNN_INTERRUPT_CLR */
#define SNN_INT_ERR         0U          /* no SNN ERR interrupt in current RTL */

/* ─── Neuron model codes — RTL NOT IMPLEMENTED as model selectors ────────── */
/*
 * The hardware uses a single fixed LIF model regardless of what is written
 * to offset 0x20.  These constants write to connect_mask[31:0], not to a
 * model selector register.  Retained for API stability.
 */
#define SNN_NEURON_LIF      0x00U       /* RTL: zeroes connect_mask[31:0] */
#define SNN_NEURON_IF       0x01U       /* RTL: connect_mask[31:0] = 0x1  */

/* ─── Hardware limits ────────────────────────────────────────────────────── */
#define SNN_MAX_NEURONS     256U        /* Fixed in RTL (NUM_NEURONS = 256) */

/* ─── Convenience register accessor ─────────────────────────────────────── */
#define SNN_REG(offset)     REG(SNN_BASE, offset)

/* ─── Driver API ─────────────────────────────────────────────────────────── */

/**
 * snn_soft_reset() - Compatibility shim. RTL NOT IMPLEMENTED.
 * CONTROL[1] is not wired to any FSM; no hardware reset exists.
 * This function waits for BUSY=0 and clears CONTROL without writing RESET.
 */
void snn_soft_reset(void);

/** snn_init() - Wait for BUSY=0, clear IRQ latch, leave SNN ready. */
void snn_init(void);

/**
 * snn_configure() - Write simulation parameters.
 *   weight_addr: ignored — this parameter is accepted for API compatibility
 *                but the synapse memory must be programmed separately via
 *                snn_write_synapse() (synapse window, see snn.h header
 *                comment and SNN_SYNAPSE_WINDOW).
 *   threshold:   LIF membrane threshold (16-bit, consumed by neuron_core)
 *   leak:        per-timestep membrane leak (16-bit, consumed by neuron_core)
 *   timesteps:   number of simulation steps (16-bit, drives timestep_counter)
 *   neuron_cfg:  written to connect_mask[31:0] (offset 0x20); it is NOT a
 *                neuron model selector — the hardware uses a single LIF model
 */
void snn_configure(uint32_t weight_addr,
                   uint32_t threshold,
                   uint32_t leak,
                   uint32_t timesteps,
                   uint32_t neuron_cfg);

/**
 * snn_write_synapse() - Write one weight byte to synapse_mem.
 * @neuron_idx: neuron index (0..255); addresses
 *              SNN_BASE + SNN_SYNAPSE_WINDOW + neuron_idx
 * @weight:     signed INT8 weight value
 * This is the correct way to preload a single synapse weight via the CPU's
 * Wishbone path (a single-byte store, sel=one-hot, to the synapse window).
 *
 * FIXED in Phase 1 (was an architectural limitation, documented as such,
 * before this fix): the synapse memory previously collided with the
 * control-register address range, and separately snn_top.sv discarded the
 * DMA path's byte-enable, so only WB single-byte writes to the one
 * coincidentally-reachable offset (old 0x0C, neuron 12) ever worked. Both
 * root causes are now fixed in synapse_mem.sv/snn_top.sv/snn_regs.sv:
 * synapse memory has its own address window (SNN_SYNAPSE_WINDOW) separate
 * from the register file, and both the WB and DMA write paths pass real
 * byte-enables through to a byte-enabled, 32-bit-wide write port. DMA CH2
 * (mem→SNN, DMA_CH_MEM_TO_SNN in dma.h) is now expected to work correctly
 * for bulk synapse preload, including burst transfers — this still needs
 * confirming against a cocotb test-snn/test-dma regression run before
 * firmware is switched over to using it for bulk loads; this function
 * remains the byte-at-a-time WB path and is unaffected either way.
 */
void snn_write_synapse(uint8_t neuron_idx, int8_t weight);

/**
 * snn_write_input() - Write one INT8 external-stimulus value to input_buffer.
 * @neuron_idx: neuron index (0..255); addresses
 *              SNN_BASE + SNN_INPUT_BUFFER_WINDOW + neuron_idx
 * @value:      signed INT8 feature value (a current, not a spike) -- added
 *              into that neuron's membrane every cycle for the duration of
 *              the run (neuron_core.sv's ext_current_i). Independent of,
 *              and in addition to, that neuron's synapse weight / self-
 *              feedback path (snn_write_synapse() / connect_mask).
 * Same byte-write technique as snn_write_synapse(), against the separate
 * input-buffer address window so there is no collision with synapse memory
 * or the register file.
 */
void snn_write_input(uint8_t neuron_idx, int8_t value);

/** snn_launch() - Write CONTROL = START (bit 0 only). Non-blocking. */
void snn_launch(void);

/**
 * snn_poll_done() - Spin on STATUS bit 1 (DONE).
 * Returns 0 on DONE.  Returns -1 on timeout only (ERR is RTL NOT IMPLEMENTED).
 */
int snn_poll_done(void);

/**
 * snn_irq_enable() - Arm SNN IRQ at the INTC level.
 * SNN has NO local interrupt-enable register; the IRQ latch is always armed.
 * Call intc_enable(IRQ_SNN) before snn_launch().
 */
void snn_irq_enable(void);

/** snn_irq_disable() - Disarm SNN IRQ at the INTC level. */
void snn_irq_disable(void);

/**
 * snn_irq_clear() - Any write to SNN_INTERRUPT_CLR clears irq_latched.
 * Must be called BEFORE intc_ack(IRQ_SNN) in the ISR.
 */
void snn_irq_clear(void);

/**
 * snn_irq_flag_clear() - Reset software ISR flags.
 * Call before each interrupt-driven snn_launch().
 */
void snn_irq_flag_clear(void);

/**
 * snn_irq_wait() - Spin-wait for interrupt-driven SNN completion.
 * Returns 0 on DONE.  Returns -1 on timeout.
 * ERR flag (_snn_irq_err) will always be 0 (ERR is RTL NOT IMPLEMENTED).
 */
int snn_irq_wait(void);

#endif /* SNN_H */
