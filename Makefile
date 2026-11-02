# Makefile — ALTair firmware build system (NPU-FREE BUILD)
# Target  : Ibex RV32IM @ 50 MHz, bare-metal (no OS, no stdlib)
# Toolchain: riscv64-unknown-elf-gcc
#
# Changes from the NPU build: src/npu.c is gone, src/conv1d.c takes its
# place, and the weight/layout files are named model_* rather than npu_*.
# Everything else about the build (flags, linker script, image generation,
# test images) is unchanged.
#
# Targets:
#   make            firmware.elf/.bin/.hex + Verilog .mem images
#   make size       size report
#   make disasm     annotated disassembly
#   make tests      one ELF per test/test_*.c
#   make tests-mem  .mem images for those test ELFs
#   make host-test  build + run the native host harness (no RISC-V
#                   toolchain needed) -- see host/README.md
#   make syntax-check
#                   parse the sources the host harness cannot link
#                   (main.c, trap.c, sensor.c, test/*.c) with the host
#                   compiler; also needs no RISC-V toolchain
#   make clean

# ── Toolchain ──────────────────────────────────────────────────────────────
CROSS   := riscv64-unknown-elf
CC      := $(CROSS)-gcc
OBJCOPY := $(CROSS)-objcopy
OBJDUMP := $(CROSS)-objdump
SIZE    := $(CROSS)-size

# ── Paths ──────────────────────────────────────────────────────────────────
ROOT      := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
SRC       := $(ROOT)src
TEST      := $(ROOT)test
BUILD     := $(ROOT)build
TELEMETRY := $(ROOT)telemetry
HOST      := $(ROOT)host

# ── Flags ──────────────────────────────────────────────────────────────────
# -nostartfiles: do not link CRT from toolchain (we supply crt0.S ourselves)
CFLAGS  := -march=rv32im_zicsr \
           -mabi=ilp32 \
           -O2 \
           -ffreestanding \
           -nostdlib \
           -nostartfiles \
           -Wall \
           -Wextra \
           -I include \
           -I telemetry

LDFLAGS := -T$(ROOT)soc.ld \
           -nostdlib \
           -nostartfiles \
           -Wl,--gc-sections

# Each link gets its own map, named after the ELF it describes:
# firmware.elf -> firmware.map, test_snn.elf -> test_snn.map. This used to
# be a fixed -Wl,-Map=$(BUILD)/firmware.map inside LDFLAGS, which the test
# rules below inherit -- so every one of the 17 test links overwrote it,
# and after `make tests` build/firmware.map actually described whichever
# test linked last, with no trace of main.o. The map is the artifact
# anyone reviewing memory layout or section placement reads, so a
# silently-wrong one is worse than none.
MAPFLAG = -Wl,-Map=$(@:.elf=.map)

# -lgcc: GCC's own runtime support library (soft 64-bit divide/multiply
# helpers such as __udivdi3), NOT libc — -nostdlib excludes it by default,
# but features.c's int64_t arithmetic (needed because a 128-sample sum of
# squares overflows 32 bits) requires it. Must be linked AFTER the object
# files that reference it, so it is appended to the link command, not
# placed in LDFLAGS (which precedes $(OBJS) in the recipes below).
LDLIBS  := -lgcc

# ── Source files ───────────────────────────────────────────────────────────
C_SRCS  := $(SRC)/main.c               \
            $(SRC)/uart.c               \
            $(SRC)/dma.c                \
            $(SRC)/snn.c                \
            $(SRC)/timer.c              \
            $(SRC)/intc.c               \
            $(SRC)/gpio.c               \
            $(SRC)/pwm.c                \
            $(SRC)/spi.c                \
            $(SRC)/i2c.c                \
            $(SRC)/inference.c          \
            $(SRC)/conv1d.c             \
            $(SRC)/sensor.c             \
            $(SRC)/features.c           \
            $(SRC)/model_weights_data.c \
            $(SRC)/latency.c            \
            $(SRC)/trap.c               \
            $(TELEMETRY)/crc16.c        \
            $(TELEMETRY)/telemetry.c

ASM_SRCS := $(SRC)/crt0.S

# ── Object files ───────────────────────────────────────────────────────────
ASM_OBJS     := $(patsubst $(SRC)/%.S,       $(BUILD)/%.o, $(ASM_SRCS))
SRC_C_OBJS   := $(patsubst $(SRC)/%.c,       $(BUILD)/%.o, $(filter $(SRC)/%, $(C_SRCS)))
TELEM_C_OBJS := $(patsubst $(TELEMETRY)/%.c, $(BUILD)/%.o, $(filter $(TELEMETRY)/%, $(C_SRCS)))
C_OBJS       := $(SRC_C_OBJS) $(TELEM_C_OBJS)

# crt0.o MUST be the first object passed to the linker so _trap_vector/
# _start land at their linker-guaranteed fixed offsets (IRAM+0x0 / IRAM+0x80,
# where IRAM = ORIGIN(IRAM) = 0x0001_0000 — see soc.ld).
OBJS     := $(ASM_OBJS) $(C_OBJS)

# ── Test support ───────────────────────────────────────────────────────────
TEST_SRCS := $(wildcard $(TEST)/test_*.c)
TEST_ELFS := $(patsubst $(TEST)/%.c, $(BUILD)/%.elf, $(TEST_SRCS))

# Objects shared by every test: crt0 + all drivers except main.o
COMMON_OBJS := $(ASM_OBJS)          \
               $(BUILD)/uart.o       \
               $(BUILD)/dma.o        \
               $(BUILD)/snn.o        \
               $(BUILD)/timer.o      \
               $(BUILD)/intc.o       \
               $(BUILD)/gpio.o       \
               $(BUILD)/pwm.o        \
               $(BUILD)/spi.o        \
               $(BUILD)/i2c.o        \
               $(BUILD)/trap.o       \
               $(BUILD)/crc16.o      \
               $(BUILD)/telemetry.o

# test_conv1d.c additionally needs the compute kernel, the feature builder
# and the weight tables; harmless for the peripheral tests to link too, but
# kept separate so a peripheral test image stays as small as it was.
CONV_OBJS   := $(BUILD)/conv1d.o      \
               $(BUILD)/features.o    \
               $(BUILD)/model_weights_data.o

# ── Primary targets ────────────────────────────────────────────────────────
.PHONY: all clean size disasm tests tests-mem hex mem host-test syntax-check

all: $(BUILD)/firmware.elf $(BUILD)/firmware.bin \
     $(BUILD)/firmware.hex \
     $(BUILD)/firmware_imem.mem $(BUILD)/firmware_dmem.mem
	@echo "Build complete → build/firmware.elf  build/firmware.bin"
	@echo "              → build/firmware.hex"
	@echo "              → build/firmware_imem.mem  build/firmware_dmem.mem"

# ── Build directory ────────────────────────────────────────────────────────
$(BUILD):
	mkdir -p $(BUILD)

# ── Compile C → object ────────────────────────────────────────────────────
$(BUILD)/%.o: $(SRC)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# ── Compile telemetry C → object ──────────────────────────────────────────
$(BUILD)/%.o: $(TELEMETRY)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# ── Assemble .S → object ──────────────────────────────────────────────────
# gcc is used as the assembler front-end so it handles .S preprocessing.
$(BUILD)/%.o: $(SRC)/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# ── Link firmware ELF ──────────────────────────────────────────────────────
$(BUILD)/firmware.elf: $(OBJS) $(ROOT)soc.ld | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $(MAPFLAG) $(OBJS) -o $@ $(LDLIBS)

# ── Binary image ──────────────────────────────────────────────────────────
$(BUILD)/firmware.bin: $(BUILD)/firmware.elf
	$(OBJCOPY) -O binary $< $@

# ── Intel HEX format ──────────────────────────────────────────────────────
# For OpenOCD, GDB, and some FPGA loaders.
$(BUILD)/firmware.hex: $(BUILD)/firmware.elf
	$(OBJCOPY) -O ihex $< $@

hex: $(BUILD)/firmware.hex
	@echo "Intel HEX → build/firmware.hex"

# ── Verilog $readmemh compatible .mem files ───────────────────────────────
# Vivado BRAM preload — IMEM (instructions) and DMEM (data) split.
$(BUILD)/firmware_imem.mem: $(BUILD)/firmware.elf
	# .data is included here via its LMA (the image the linker places in
	# IRAM right after .rodata -- see soc.ld) even though instr_mem.sv only
	# ever backs IRAM, not DRAM. Verilator's IRAM model is the only memory
	# actually preloaded via $readmemh (FIRMWARE_FILE); DRAM (data_mem.sv)
	# has no preload path at all. Without .data's LMA bytes present here,
	# crt0.S's .data_loop faithfully copies zeros (IRAM's default reset
	# content) into every non-zero-initialised global instead of its real
	# initialiser. Real hardware/FPGA is unaffected (firmware.bin/.hex are
	# unfiltered, flat images). Matches how .srodata was folded into
	# .rodata for the same reason (see soc.ld's .rodata comment).
	$(OBJCOPY) -O verilog --only-section=.vectors \
	           --only-section=.text \
	           --only-section=.rodata \
	           --only-section=.data $< $@

$(BUILD)/firmware_dmem.mem: $(BUILD)/firmware.elf
	$(OBJCOPY) -O verilog --only-section=.data $< $@

mem: $(BUILD)/firmware_imem.mem $(BUILD)/firmware_dmem.mem
	@echo "Memory files → build/firmware_imem.mem build/firmware_dmem.mem"

# ── Size report ───────────────────────────────────────────────────────────
size: $(BUILD)/firmware.elf
	$(SIZE) $(BUILD)/firmware.elf

# ── Disassembly ───────────────────────────────────────────────────────────
disasm: $(BUILD)/firmware.elf
	$(OBJDUMP) -d -S $(BUILD)/firmware.elf > $(BUILD)/firmware.lst
	@echo "Disassembly → build/firmware.lst"

# ── Test builds ───────────────────────────────────────────────────────────
# Each test_*.c has its own main(); link it with crt0 + driver objects
# but NOT with the firmware main.o.
tests: $(TEST_ELFS)
	@echo "All test ELFs built."

$(BUILD)/test_conv1d.elf: $(TEST)/test_conv1d.c $(COMMON_OBJS) $(CONV_OBJS) $(ROOT)soc.ld | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $(MAPFLAG) $(COMMON_OBJS) $(CONV_OBJS) $< -o $@ $(LDLIBS)

$(BUILD)/test_%.elf: $(TEST)/test_%.c $(COMMON_OBJS) $(ROOT)soc.ld | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $(MAPFLAG) $(COMMON_OBJS) $< -o $@ $(LDLIBS)

# ── Test $readmemh-compatible memory files ────────────────────────────────
$(BUILD)/test_%_imem.mem: $(BUILD)/test_%.elf
	# .data included via LMA -- see firmware_imem.mem's rule above for why.
	$(OBJCOPY) -O verilog --only-section=.vectors \
	           --only-section=.text \
	           --only-section=.rodata \
	           --only-section=.data $< $@

$(BUILD)/test_%_dmem.mem: $(BUILD)/test_%.elf
	$(OBJCOPY) -O verilog --only-section=.data $< $@

TEST_IMEM_MEMS := $(patsubst $(TEST)/test_%.c, $(BUILD)/test_%_imem.mem, $(TEST_SRCS))
TEST_DMEM_MEMS := $(patsubst $(TEST)/test_%.c, $(BUILD)/test_%_dmem.mem, $(TEST_SRCS))

tests-mem: $(TEST_IMEM_MEMS) $(TEST_DMEM_MEMS)
	@echo "All test *_imem.mem/*_dmem.mem built."

# ── Native host harness ───────────────────────────────────────────────────
# Builds and runs the same firmware sources against a software device
# model with the host compiler. Needs no RISC-V toolchain. This is a
# software fixture: it checks the firmware's integer pipeline and framing,
# and proves nothing about RTL behaviour.
host-test:
	$(MAKE) -C $(HOST) run

syntax-check:
	$(MAKE) -C $(HOST) syntax-check

# ── Clean ──────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD)/
	$(MAKE) -C $(HOST) clean
	@echo "build/ removed."
