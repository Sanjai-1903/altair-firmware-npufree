/*
 * snn.c — Spiking Neural Network accelerator driver
 * No stdlib / POSIX headers.
 *
 * Verified RTL contract (snn_regs.sv, snn_ctrl.sv, snn_top.sv):
 *
 *   CONTROL[0] = START  — drives snn_ctrl.start; self-clears in snn_regs.sv
 *                         on the busy-falling-edge (Phase 8 fix — mirrors
 *                         npu_regs.sv's BUG 3 FIX; previously never
 *                         cleared at all, so the FSM auto-restarted every
 *                         ~timesteps+3 cycles forever after one snn_launch()).
 *   CONTROL[1] = RESET  — RTL NOT IMPLEMENTED (stored; never read by RTL)
 *   STATUS[0]  = BUSY   — driven by snn_ctrl (1 during S_EXEC)
 *   STATUS[1]  = DONE   — sticky latch (snn_top.sv `done_latched`, Phase 8
 *                         fix): set on snn_ctrl's completion pulse, held
 *                         until the next snn_launch() clears it. Separate
 *                         from the IRQ-ack latch (irq_latched) so a
 *                         concurrent ISR clearing the interrupt can't race
 *                         a foreground snn_poll_done() out of seeing it.
 *   STATUS[2]  = INIT_DONE — all init-tracked SNN memories have been written
 *
 *   Synapse memory window [SNN_BASE+0x100 .. SNN_BASE+0x1FF] (Phase 1 fix):
 *          256 byte-addressable entries, one per neuron. Separate address
 *          window from the control/status/config registers below (decoded
 *          via wb_addr_i[8] in snn_top.sv) -- previously collided with
 *          those registers when synapse writes used offset = neuron_idx
 *          directly. See snn_write_synapse().
 *
 *   Output spike buffer [0x50..0x6C] (Phase 1 addition): 8x32-bit words,
 *          read-only, live spike_vector from the neuron array.
 *
 *   0x14 THRESHOLD     — 16-bit; consumed by neuron_core (LIF fire level)
 *   0x18 LEAK          — 16-bit; consumed by neuron_core (per-timestep decay)
 *   0x1C TIMESTEPS     — 16-bit; drives timestep_counter in snn_top.sv
 *   0x20 CONNECT_MASK_0 — connect_mask[31:0]; NOT a neuron model selector
 *   0x28 INTERRUPT_CLR  — any write clears irq_latched (no EN register)
 *   0x30–0x4C CONNECT_MASK_1..7 — connect_mask[255:32] in 7 × 32-bit words
 *
 *   Dead RTL (NOT instantiated in snn_top.sv):
 *     spike_mem.sv, synapse_core.sv
 */

#include "snn.h"

/* ISR status flags defined in trap.c; written only from M-mode trap context */
extern volatile uint32_t _snn_irq_done;
extern volatile uint32_t _snn_irq_err;   /* RTL NOT IMPLEMENTED — always 0 */

/* ── Public API ──────────────────────────────────────────────────────────── */

void snn_soft_reset(void)
{
    /*
     * RTL NOT IMPLEMENTED — CONTROL[1] (RESET bit) is stored in control_reg
     * but snn_ctrl.sv and all other SNN modules never read it.  Writing it
     * has no hardware effect on the FSM, memory, or neuron state.
     *
     * Safe behavior: wait for BUSY=0, then clear CONTROL to ensure no stale
     * START bit is left asserted before the next configure/launch sequence.
     */
    while (SNN_REG(SNN_STATUS) & SNN_STAT_BUSY)
        ;                           /* wait for any in-progress operation */
    SNN_REG(SNN_CONTROL) = 0U;     /* clear residual control bits        */
}

void snn_init(void)
{
    snn_soft_reset();
    /* Clear IRQ latch: any write to 0x28 clears irq_latched */
    SNN_REG(SNN_INTERRUPT_CLR) = 1U;

    /*
     * Zero input_buffer[0..255]. input_buffer.sv has no rst_n-driven reset
     * (same convention as synapse_mem.sv) -- on real silicon/FPGA its
     * power-up content is unspecified, unlike Verilator/cocotb simulation
     * where an uninitialized array conventionally reads as 0. Without this
     * explicit zero-fill, ext_current_i could be a nonzero garbage value on
     * hardware even for callers that never touch the input buffer at all,
     * silently perturbing the existing self-feedback-only behavior that
     * snn_write_synapse()-based callers (e.g. test_snn.c) depend on.
     */
    {
        uint32_t i;
        for (i = 0U; i < SNN_MAX_NEURONS; i++) {
            snn_write_input((uint8_t)i, 0);
            snn_write_synapse((uint8_t)i, 0);
        }
    }
}

void snn_configure(uint32_t weight_addr,
                   uint32_t threshold,
                   uint32_t leak,
                   uint32_t timesteps,
                   uint32_t neuron_cfg)
{
    /*
     * weight_addr: NOT written to any register.
     *   Synapse memory lives in its own address window (SNN_SYNAPSE_WINDOW,
     *   Phase 1 fix), not the control-register range.
     *   The caller is responsible for preloading synapse weights separately
     *   via snn_write_synapse().
     *
     * threshold:  written to SNN_THRESHOLD (0x14)   — functional (neuron_core)
     * leak:       written to SNN_LEAK (0x18)         — functional (neuron_core)
     * timesteps:  written to SNN_TIMESTEPS (0x1C)    — functional (timestep_counter)
     *
     * neuron_cfg: written to SNN_CONNECT_MASK_0 (0x20) = connect_mask[31:0].
     *   This is NOT a neuron model selector.  The hardware implements a single
     *   fixed LIF model regardless of this value.  The value controls which
     *   of the first 32 neurons are connected via spike_router.
     *   SNN_NEURON_LIF (0x00) disconnects the first 32 neurons.
     *   SNN_NEURON_IF  (0x01) enables only neuron 0 in that slice.
     *   Default after rst_n: connect_mask = all-ones (all connected).
     */
    (void)weight_addr;                          /* NOT written — see above */
    SNN_REG(SNN_THRESHOLD)        = threshold;  /* functional */
    SNN_REG(SNN_LEAK)             = leak;        /* functional */
    SNN_REG(SNN_TIMESTEPS)        = timesteps;   /* functional */
    SNN_REG(SNN_CONNECT_MASK_0)   = neuron_cfg;  /* connect_mask[31:0] */
}

void snn_write_synapse(uint8_t neuron_idx, int8_t weight)
{
    /*
     * Write one INT8 weight byte to synapse_mem at address neuron_idx.
     * RTL (Phase 1 fix): synapse memory has its own address window,
     * SNN_BASE + SNN_SYNAPSE_WINDOW + neuron_idx, decoded in snn_top.sv
     * via wb_addr_i[8] -- separate from the control/status/config register
     * range at SNN_BASE + 0x00..0x6C. A single-byte store here produces
     * sel=one-hot / addr=neuron_idx within the window, which snn_top.sv's
     * WB routing mux now passes through (be, full address, full word) to
     * synapse_mem's byte-enabled write port.
     *
     * This requires writing via a pointer to the absolute address
     * SNN_BASE + SNN_SYNAPSE_WINDOW + neuron_idx, not via SNN_REG() (which
     * only ever adds a fixed offset, not neuron_idx).
     */
    volatile uint8_t *syn_port = SOC_BYTE_PORT(
        SNN_BASE + SNN_SYNAPSE_WINDOW + (uint32_t)neuron_idx);
    *syn_port = (uint8_t)weight;
}

void snn_write_input(uint8_t neuron_idx, int8_t value)
{
    /*
     * Write one INT8 external-stimulus value to input_buffer at address
     * neuron_idx. Mirrors snn_write_synapse() exactly, against the
     * separate input-buffer window (SNN_BASE + SNN_INPUT_BUFFER_WINDOW +
     * neuron_idx, wb_addr_i[9:8]==2'b10 in snn_top.sv) so there is no
     * collision with the synapse window or register file.
     */
    volatile uint8_t *in_port = SOC_BYTE_PORT(
        SNN_BASE + SNN_INPUT_BUFFER_WINDOW + (uint32_t)neuron_idx);
    *in_port = (uint8_t)value;
}

void snn_launch(void)
{
    /* Write only the START bit — CONTROL[0] only.
     * CONTROL[1] (RESET) is not wired to the FSM; do not write it here. */
    SNN_REG(SNN_CONTROL) = SNN_CTRL_START;
}

int snn_poll_done(void)
{
    extern volatile uint32_t _snn_irq_done;
    uint32_t st;
    uint32_t timeout = 0x100000UL;   /* ~2M cycles @ 50 MHz ≈ 40 ms */

    do {
        st = SNN_REG(SNN_STATUS);
        if (_snn_irq_done) { return 0; }
        if (--timeout == 0U)
            return -1;               /* timeout — DONE never asserted */
    } while (!(st & SNN_STAT_DONE));

    (void)st;
    return 0;
}

void snn_irq_enable(void)
{
    /* SNN has NO local interrupt-enable register.
     * irq_o = irq_latched — always armed.  Arm at the INTC side only. */
    intc_enable(IRQ_SNN);
}

void snn_irq_disable(void)
{
    intc_disable(IRQ_SNN);
}

void snn_irq_clear(void)
{
    /* Any write to SNN_INTERRUPT_CLR clears irq_latched to 0.
     * The value written does not matter. */
    SNN_REG(SNN_INTERRUPT_CLR) = 1U;
}

void snn_irq_flag_clear(void)
{
    /* Reset software flags before each interrupt-driven launch.
     * _snn_irq_err will remain 0 (ERR is RTL NOT IMPLEMENTED). */
    _snn_irq_done = 0U;
    _snn_irq_err  = 0U;
}

int snn_irq_wait(void)
{
    uint32_t timeout = 0x100000UL;   /* matches snn_poll_done() — ~40 ms */

    while (!_snn_irq_done && !_snn_irq_err) {
        if (--timeout == 0U)
            return -1;
    }
    /* _snn_irq_err will never be 1 (ERR RTL NOT IMPLEMENTED) */
    return _snn_irq_err ? -1 : 0;
}
