/* =============================================================================
 * soc.ld - GNU LD Linker Script for Edge-AI RISC-V SoC
 * CPU  : Ibex RV32IM
 * IRAM : 0x00010000, 64 KB  (Instruction SRAM)
 * DRAM : 0x10000000, 64 KB  (Data SRAM)
 *
 * Boot ROM architecture (2026-07-10): a hardware boot_rom (SoC/rtl/memory/
 * boot_rom.sv) is now mapped at 0x00000000 and is what Ibex actually
 * fetches first after reset (boot_addr=BOOT_ROM_BASE=0x0, hardwired reset
 * PC = boot_addr+0x80 -> boot_rom+0x80). boot_rom.hex contains a
 * two-instruction hardware trampoline at that offset
 * (`lui x31,0x10000; jalr x0,0x80(x31)`) that jumps unconditionally to
 * 0x00010080 (IRAM's new origin + 0x80). This replaces the previous
 * firmware-only single-instruction relay trampoline (_reset_vector), which
 * has been removed - see firmware/src/crt0.S.
 * ============================================================================= */

/* -----------------------------------------------------------------------------
 * OUTPUT_ARCH: tells the linker this is a 32-bit RISC-V ELF binary.
 * The toolchain is riscv32-unknown-elf-gcc so the arch string must match.
 * ----------------------------------------------------------------------------- */
OUTPUT_ARCH(riscv)

/* -----------------------------------------------------------------------------
 * ENTRY: the very first instruction of firmware the CPU executes.
 *
 * Execution reaches here via the Boot ROM's hardware trampoline (see file
 * header above), which jumps unconditionally to IRAM+0x80. _start is
 * pinned to exactly that address by the .vectors section below to match
 * the Boot ROM's hardcoded jump target; it is the real startup sequence
 * itself now (stack init, .data copy, .bss zero, mtvec install - see
 * crt0.S), not a relay jump.
 * ----------------------------------------------------------------------------- */
ENTRY(_start)

/* =============================================================================
 * MEMORY REGIONS
 * Each region defines a named address window.
 * Attributes:
 *   r = readable
 *   w = writable
 *   x = executable
 *   a = allocatable
 *   ! = NOT
 * ============================================================================= */
MEMORY
{
    /* Instruction SRAM - executable, readable, NOT writable at runtime.
     * Holds .text and .rodata.  Also stores the LMA copy of .data that
     * crt0.S will copy into DRAM before main() runs.                     */
    IRAM (rx)  : ORIGIN = 0x00010000, LENGTH = 64K

    /* Data SRAM - readable + writable, NOT executable.
     * Holds .data (VMA), .bss, stack.
     * Accessible by CPU, DMA, NPU, and SNN (per address map spec).        */
    DRAM (rw!x): ORIGIN = 0x10000000, LENGTH = 64K
}

SECTIONS
{
    /* -------------------------------------------------------------------------
     * .vectors - Hardware-facing entry points (reset and traps).
     * Must be placed precisely where the Boot ROM hardware expects them.
     * ------------------------------------------------------------------------- */
    .vectors :
    {
        /* _trap_vector (crt0.S): fixed-address `j machine_trap_handler`
         * trampoline for the hardware Boot ROM to blindly jump to on a
         * synchronous exception (offset 0x0) or machine-external-
         * interrupt cause (offset 0x2C = 4*11) - see boot_rom.hex. Placed
         * FIRST, at IRAM+0x0, reusing the low padding that already
         * existed ahead of _start's own fixed +0x80 placement below. */
        . = ORIGIN(IRAM) + 0x0;
        _trap_vector_start = .;
        KEEP(*(.trap_vectors))

        . = ORIGIN(IRAM) + 0x80;       /* absolute location counter, not region-relative */
        _vectors_start = .;
        KEEP(*(.vectors))               /* _start (crt0.S): real startup sequence */

        _vectors_end = .;
    } > IRAM                           /* VMA = LMA = IRAM */

    ASSERT(_trap_vector_start == ORIGIN(IRAM) + 0x0,
           "trap vector trampoline is not at IRAM+0x0 (Boot ROM's vectored interrupt table jumps unconditionally there for both exceptions and the machine-external-interrupt cause -- see boot_rom.hex)")
    ASSERT(_vectors_start == ORIGIN(IRAM) + 0x80,
           "firmware entry point is not at IRAM+0x80 (Boot ROM's hardware trampoline jumps unconditionally to IRAM+0x80 -- see boot_rom.hex)")

    /* -------------------------------------------------------------------------
     * .text - All executable code.
     * Placed immediately after .vectors in IRAM.
     * ------------------------------------------------------------------------- */
    .text :
    {
        . = ALIGN(4);                  /* keep 4-byte (word) alignment for RV32 */
        *(.text)                       /* normal code sections */
        *(.text.*)                     /* compiler-generated subsections (.text.foo) */
        . = ALIGN(4);                  /* realign after last object */
    } > IRAM

    /* -------------------------------------------------------------------------
     * .rodata - Read-only data (string literals, const globals, lookup tables).
     * Stored in IRAM alongside code; no copy needed at startup.
     *
     * Also absorbs .srodata* : the RISC-V toolchain's small-data optimization
     * (-G threshold, on by default) emits small const objects (e.g. a single
     * `const unsigned int`) into a SEPARATE `.srodata` ELF section rather than
     * `.rodata`, for gp-relative addressing. Without this, .srodata is placed
     * as an unnamed orphan section by default linker heuristics - invisible
     * to any tool (or Makefile objcopy --only-section filter) that expects
     * "read-only data" to mean only `.rodata`. Confirmed 2026-07-10: this
     * silently dropped g_npu_weights_len (firmware/src/npu_weights_data.c)
     * from the Verilator simulation memory image (firmware_imem.mem only
     * copied .vectors/.text/.rodata), making inference_init()'s weight
     * preload loop a no-op in simulation only (real hardware .bin/.hex is
     * unaffected, since those are generated from the whole ELF, unfiltered).
     * ------------------------------------------------------------------------- */
    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)                     /* standard read-only data */
        *(.rodata.*)                   /* compiler subsections */
        *(.srodata)                    /* small-data-optimized read-only objects */
        *(.srodata.*)
        . = ALIGN(4);
    } > IRAM

    /* -------------------------------------------------------------------------
     * .data - Initialised global/static variables.
     *
     * VMA (runtime address) is in DRAM so the CPU can write to these vars.
     * LMA (load address / storage address) is in IRAM, immediately after
     * .rodata, so the firmware image is a single flat binary in IRAM.
     *
     * crt0.S uses the three exported symbols to perform the copy loop:
     *   _data_load  - source pointer (LMA, in IRAM)
     *   _data_start - destination start (VMA, in DRAM)
     *   _data_end   - destination end   (VMA, in DRAM)
     * ------------------------------------------------------------------------- */
    .data :
    {
        . = ALIGN(4);
        _data_start = .;               /* exported: crt0 copy-loop destination start */
        *(.data)                       /* initialised globals */
        *(.data.*)                     /* compiler subsections */
        *(.sdata)                      /* small-data-optimized initialised globals (see .rodata comment) */
        *(.sdata.*)
        . = ALIGN(4);
        _data_end = .;                 /* exported: crt0 copy-loop destination end */
    } > DRAM AT > IRAM                 /* VMA->DRAM, LMA stored in IRAM */

    /* _data_load is the LMA (IRAM address) where the initialised data image
     * is stored in flash / IRAM before crt0.S copies it to DRAM.           */
    _data_load = LOADADDR(.data);      /* exported: crt0 copy-loop source pointer */

    /* -------------------------------------------------------------------------
     * .bss - Zero-initialised globals and statics.
     *
     * NOLOAD tells the linker to allocate address space but emit NO bytes
     * in the output binary.  crt0.S zeroes this region at startup using
     * _bss_start and _bss_end before calling main().
     * ------------------------------------------------------------------------- */
    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _bss_start = .;                /* exported: crt0 zero-fill loop start */
        *(.bss)                        /* uninitialised globals */
        *(.bss.*)                      /* compiler subsections */
        *(.sbss)                       /* small-data-optimized uninitialised globals (see .rodata comment) */
        *(.sbss.*)
        *(COMMON)                      /* tentative definitions (C common symbols) */
        . = ALIGN(4);
        _bss_end = .;                  /* exported: crt0 zero-fill loop end */
    } > DRAM                           /* VMA in DRAM; no LMA bytes emitted */

    /* _stack_bottom sits immediately after .bss in DRAM.
     * crt0.S fills from here to _stack_top with 0xDEADBEEF for
     * worst-case stack-depth watermarking.                            */
    _stack_bottom = _bss_end;          /* exported: crt0 watermark fill start */

    /* -------------------------------------------------------------------------
     * Stack - grows downward from the top of DRAM.
     *
     * DRAM is 64 KB -> top of DRAM = 0x10000000 + 0x10000 = 0x1000_FFFF + 1
     *                                                     = 0x1001_0000
     *
     * _stack_top is set to the last byte of DRAM + 1 (one past the end),
     * which is the conventional "empty descending" initial SP value for
     * ARM/RISC-V ABIs.  crt0.S loads this into x2 (sp) as its first act.
     *
     * No explicit NOLOAD section is needed; the stack is purely runtime.
     * ------------------------------------------------------------------------- */
    _stack_top = ORIGIN(DRAM) + LENGTH(DRAM); /* exported: crt0 sp initialisation */

    /* -------------------------------------------------------------------------
     * Sanity guards - abort the link if sections overflow their regions.
     * ------------------------------------------------------------------------- */
    ASSERT(_data_end   <= ORIGIN(DRAM) + LENGTH(DRAM),
           "ERROR: .data overflows DRAM")
    ASSERT(_bss_end    <= ORIGIN(DRAM) + LENGTH(DRAM),
           "ERROR: .bss overflows DRAM")
    ASSERT(_stack_bottom <= _stack_top,
           "ERROR: _stack_bottom exceeds _stack_top - no room for stack")
    ASSERT(LOADADDR(.data) + SIZEOF(.data) <= ORIGIN(IRAM) + LENGTH(IRAM),
           "ERROR: .data LMA image overflows IRAM")

    /* -------------------------------------------------------------------------
     * /DISCARD/ - drop sections that have no meaning in a bare-metal build.
     * Keeping them wastes IRAM and can confuse objcopy when generating a
     * raw binary.
     * ------------------------------------------------------------------------- */
    /DISCARD/ :
    {
        *(.eh_frame)                   /* C++ exception unwinding tables */
        *(.eh_frame_hdr)               /* exception frame header */
        *(.note.*)                     /* ELF note sections */
        *(.comment)                    /* compiler version strings */
        *(.ARM.*)                      /* ARM-specific attributes (wrong arch, safety) */
    }
}
/* ============================================================================= */