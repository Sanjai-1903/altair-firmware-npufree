/* crt0.S — Startup code for Ibex RV32IM, M-mode bare-metal
 *
 * Boot sequence (2026-07-10, Boot ROM architecture):
 *   Ibex resets to boot_addr+0x80 (hardwired, ibex_if_stage.sv PC_BOOT
 *   case). soc_top now instantiates a hardware boot_rom at BOOT_ROM_BASE
 *   (0x0), so the CPU's true first fetch is boot_rom+0x80. boot_rom.hex
 *   contains a two-instruction hardware trampoline there
 *   (`lui x31,0x10000; jalr x0,0x80(x31)`) that jumps unconditionally to
 *   INSTR_MEM_BASE+0x80 (0x00010080) — i.e. hardware, not firmware, now
 *   performs the reset-vector hop.
 *
 *   Because that hardware jump target is hardcoded to +0x80 in
 *   boot_rom.hex (not just to IRAM's origin), _start below is still
 *   pinned to IRAM+0x80 by the linker script's .vectors section — but it
 *   is no longer a one-instruction relay. It IS the real startup
 *   sequence directly; the old firmware-only single-instruction
 *   trampoline (`_reset_vector: j _real_start`) is obsolete now that the
 *   Boot ROM performs the hop in hardware, and has been removed.
 *
 * Linker script exports:
 *   _stack_top    — top of DRAM stack (0x1001_0000)
 *   _stack_bottom — bottom of stack (= _bss_end), for watermark fill
 *   _data_load    — LMA of .data image in IRAM (source for copy)
 *   _data_start   — VMA start of .data in DRAM (destination)
 *   _data_end     — VMA end   of .data in DRAM
 *   _bss_start    — start of .bss in DRAM
 *   _bss_end      — end   of .bss in DRAM
 *
 * Rules enforced:
 *   - No .option relax  (Ibex linker relaxation is unreliable)
 *   - No compressed instructions  (RV32IM, not RV32IMC)
 *   - la = load address (auipc+addi pair), never lw (dereference)
 *
 * Trap vector trampoline (2026-07-11, Phase 8 interrupt fix):
 *   This vendored Ibex build's mtvec CSR is HARDWARE-INIT-ONLY — see
 *   SoC/third_party/ibex/rtl/ibex_cs_registers.sv ("mtvec_en =
 *   csr_mtvec_init_i") and ibex_if_stage.sv ("csr_mtvec_init_o =
 *   (pc_mux_i == PC_BOOT) & pc_set_i"). A normal `csrw mtvec, ...`
 *   instruction is silently ignored by the hardware; mtvec is permanently
 *   fixed at {BOOT_ADDR[31:8], 6'b0, 2'b01} = 0x00000001 (BOOT_ADDR=0),
 *   i.e. VECTORED mode with BASE=0x00000000 (Boot ROM's own address), not
 *   the direct-mode mtvec=machine_trap_handler every earlier phase's
 *   comments assumed. This was never caught before Phase 8 because no
 *   firmware before it ever exercised a genuine hardware interrupt
 *   end-to-end (earlier DMA/NPU/SNN "interrupt" code paths were only
 *   ever reached via polling in practice).
 *
 *   In vectored mode: synchronous exceptions jump to BASE (offset 0x0);
 *   interrupts jump to BASE + 4*cause. This SoC's INTC only ever drives
 *   Ibex's irq_external_i (cause 11) — irq_software_i/irq_timer_i/
 *   irq_fast_i are hardwired to 0 in cpu_wrapper.sv — so only two Boot
 *   ROM vector slots matter: offset 0x00 (exceptions) and offset 0x2C
 *   (cause 11 = machine external interrupt, 4*11). boot_rom.hex now
 *   contains a `lui x31,0x10; jalr x0,0(x31)` pair at BOTH of those
 *   offsets, jumping unconditionally to _trap_vector below — the exact
 *   same hand-encoded instruction pattern already used for the existing
 *   reset trampoline at Boot ROM +0x80 (see soc.ld's ENTRY comment).
 *
 *   _trap_vector itself must be a plain `j machine_trap_handler`
 *   (JAL x0, offset) — rd=x0 so it writes no register at all, unlike a
 *   `la`+`jr` sequence, which would clobber a register the interrupted
 *   code was live in BEFORE machine_trap_handler's own compiler-generated
 *   prologue gets a chance to save it. JAL's +/-1 MiB reach comfortably
 *   covers the distance from IRAM+0x0 to machine_trap_handler (same 64 KB
 *   IRAM region).
 *
 *   Placed at a FIXED address (IRAM+0x0, in soc.ld's .vectors section,
 *   reusing the same previously-unused low padding _start's own +0x80
 *   placement already left behind) so every firmware image guarantees
 *   the SAME jump target regardless of where machine_trap_handler itself
 *   ends up landing in that specific build — exactly mirroring how Boot
 *   ROM's reset trampoline relies on _start always being at IRAM+0x80.
 * ─────────────────────────────────────────────────────────────────────── */
    .section .trap_vectors, "ax", @progbits
    .global  _trap_vector
    .type    _trap_vector, @function

    /* ── TEMPORARY DEBUG (2026-07-19) ────────────────────────────────────
     * A single-shot capture (previous version of this block) showed trap #1
     * as mepc=0 mcause=0 mtval=0 mstatus=0x80 — none of which maps to any
     * address in inference_init()'s loop, despite 60+ ms of clean,
     * in-order UART output (through "[dbg] i=0") proving no trap occurred
     * before this point. That result is ambiguous: either a genuinely
     * distinct exception (cause 0 = instruction address misaligned)
     * precedes the illegal-instruction loop at 0x110d4/0x110ec, or this
     * capture code itself is racing the CSR write and reading stale/reset
     * values. A single sample can't tell those apart.
     *
     * Upgraded to an 8-entry ring buffer logging every trap entry (not
     * just the first) to memory, before machine_trap_handler's own prologue
     * runs. If entry 0 differs from entries 1-7, that confirms a real,
     * distinct first exception. If all 8 are identical, that indicts this
     * capture mechanism instead of the RTL/firmware.
     *
     * Register discipline unchanged from the previous version: t0-t2 are
     * saved/restored around this block so machine_trap_handler's prologue
     * still sees pristine pre-trap register values.
     * Remove this block once the first-fault root cause is identified.
     * ─────────────────────────────────────────────────────────────────── */
    .extern dbg_trap_count
    .extern dbg_trap_log

_trap_vector:
    /* Note: only 32 instructions (0x80 bytes) total fit in this section —
     * .vectors (_start) is fixed by the linker at IRAM+0x80 (soc.ld:108).
     * This block is 31 instructions / 124 bytes — 1 slot of margin. */
    addi    sp, sp, -12
    sw      t0, 0(sp)
    sw      t1, 4(sp)
    sw      t2, 8(sp)

    la      t1, dbg_trap_count
    lw      t0, 0(t1)
    li      t2, 8
    bge     t0, t2, .dbg_trap_skip    /* buffer full — stop logging        */

    slli    t2, t0, 4                 /* t2 = index * 16 (4 words/entry)   */
    la      t1, dbg_trap_log
    add     t1, t1, t2                /* t1 = &dbg_trap_log[index]         */

    csrr    t0, mepc
    sw      t0, 0(t1)
    csrr    t0, mcause
    sw      t0, 4(t1)
    csrr    t0, mtval
    sw      t0, 8(t1)
    csrr    t0, mstatus
    sw      t0, 12(t1)

    la      t1, dbg_trap_count
    lw      t0, 0(t1)
    addi    t0, t0, 1
    sw      t0, 0(t1)

.dbg_trap_skip:
    lw      t0, 0(sp)
    lw      t1, 4(sp)
    lw      t2, 8(sp)
    addi    sp, sp, 12
    /* ── END TEMPORARY DEBUG ─────────────────────────────────────────── */

    j       machine_trap_handler

    .size _trap_vector, . - _trap_vector

    /* ── Entry point — placed at absolute IRAM+0x80 by soc.ld's .vectors
     * section, matching the Boot ROM hardware trampoline's fixed jump
     * target (see file header). No firmware-side relay jump needed
     * anymore: the Boot ROM lands directly here.
     * ─────────────────────────────────────────────────────────────────── */
    .section .vectors, "ax", @progbits
    .global  _start
    .type    _start, @function

_start:
    /* ── 1. Initialise stack pointer ─────────────────────────────────────
     * la loads the *address* of the symbol (auipc + addi).
     * Using lw here would dereference _stack_top as a memory location —
     * that is wrong and would load garbage or fault.
     * ─────────────────────────────────────────────────────────────────── */
    la      x2, _stack_top          /* sp = _stack_top = 0x1001_0000     */

    /* ── 2. Copy .data from LMA (IRAM) to VMA (DRAM) ────────────────────
     * _data_load  = LOADADDR(.data)  — address of image in IRAM
     * _data_start = VMA start in DRAM
     * _data_end   = VMA end   in DRAM
     * ─────────────────────────────────────────────────────────────────── */
    la      t0, _data_load          /* source: LMA in IRAM               */
    la      t1, _data_start         /* dest start: VMA in DRAM           */
    la      t2, _data_end           /* dest end:   VMA in DRAM           */
    beq     t1, t2, .bss_init       /* skip if .data section is empty    */
.data_loop:
    lw      t3, 0(t0)               /* load word from IRAM               */
    sw      t3, 0(t1)               /* store word to DRAM                */
    addi    t0, t0, 4
    addi    t1, t1, 4
    blt     t1, t2, .data_loop

    /* ── 3. Zero-fill .bss ───────────────────────────────────────────────
     * All words from _bss_start to _bss_end are cleared to 0.
     * ─────────────────────────────────────────────────────────────────── */
.bss_init:
    la      t0, _bss_start
    la      t1, _bss_end
    beq     t0, t1, .call_main      /* skip if .bss is empty             */
.bss_loop:
    sw      x0, 0(t0)               /* store zero word                   */
    addi    t0, t0, 4
    blt     t0, t1, .bss_loop

    /* ── 4. Fill stack region with 0xDEADBEEF watermark ─────────────────────
     * At runtime, scan from _stack_bottom upward; count words still equal
     * to the pattern to find worst-case stack depth (unused = still intact).
     * Pattern: 0xDEADBEEF = 3735928559
     * ─────────────────────────────────────────────────────────────────────── */
.stack_fill:
    la   t0, _stack_bottom          /* load bottom of stack region     */
    la   t1, _stack_top             /* load top of stack               */
    li   t2, 0xDEADBEEF             /* watermark pattern               */
.stack_fill_loop:
    sw   t2, 0(t0)                  /* write pattern word              */
    addi t0, t0, 4                  /* advance by one word             */
    blt  t0, t1, .stack_fill_loop  /* loop until top                  */

    /* ── 5. Machine trap handler is already installed — in hardware ────────
     * No software mtvec write is possible on this Ibex build (see the
     * "Trap vector trampoline" note in this file's header): mtvec is
     * fixed at boot to Boot ROM's vectored table (BASE=0x0), which now
     * routes both synchronous exceptions and the machine-external-
     * interrupt cause through _trap_vector (IRAM+0x0) to
     * machine_trap_handler. A `csrw mtvec, ...` here would be silently
     * ignored by the hardware, so it has been removed rather than left as
     * dead code that implies a software-writable mtvec which does not
     * exist on this core.
     * ─────────────────────────────────────────────────────────────────────── */

    /* ── 6. Call main ────────────────────────────────────────────────────
     * call = auipc ra, + jalr ra  (two-instruction pseudo-op, no relax)
     * ─────────────────────────────────────────────────────────────────── */
.call_main:
    call    main

    /* ── 5. Hang — main must never return on bare metal ──────────────── */
.hang:
    j       .hang

    .size _start, . - _start
