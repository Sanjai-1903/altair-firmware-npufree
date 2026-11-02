# ALTair firmware — NPU-free build

Complete bare-metal firmware for the ALTair telemetry co-processor, built
from the Edge-AI SoC firmware with the **NPU block removed**. The deployed
model is unchanged; the two Conv1D layers that used to run as 32 NPU
launches now run on the Ibex CPU, producing bit-identical results.

Target: Ibex RV32IM @ 50 MHz, M-mode only, no OS, no stdlib, no float.

## What changed

The short version: `src/npu.c` and `include/npu.h` are gone;
`src/conv1d.c` and `include/conv1d.h` take their place. The NPU was never
a network accelerator — per launch it did exactly one fixed 8×32-by-32×8
INT8 matmul, and every other part of the model (im2col tiling, bias, ReLU,
requantization, pooling, layer sequencing) was already firmware. So the
removal replaces one integer matmul kernel, not a model.

`docs/NPU_REMOVAL.md` is the full change record: the equivalence argument,
the cost, the symbol renames, the address-map deltas, and the open items.

## Layout

```
Makefile                 RISC-V cross build (firmware, test images, .mem)
soc.ld                   linker script — IRAM 0x00010000, DRAM 0x10000000
include/                 driver + model headers
  conv1d.h               the software Conv1D engine (replaces npu.h)
  model_layout.h         frozen model shape + fixed-point contract
  inference.h            pipeline API and error-stage codes
  snn.h                  the only accelerator left in the build
src/
  conv1d.c               the kernel that replaced the NPU
  inference.c            pipeline orchestration
  features.c             quantization, im2col tiling, pooling
  model_weights_data.c   generated weight tiles + biases
  main.c crt0.S trap.c   boot, trap handling, top-level loop
  uart.c intc.c timer.c dma.c gpio.c spi.c i2c.c pwm.c snn.c sensor.c
  latency.c
telemetry/               framed packet output + CRC16
test/                    one bare-metal image per peripheral; test_conv1d.c
host/                    native harness — build and test with no RISC-V
                         toolchain (fixture only; see host/README.md)
docs/NPU_REMOVAL.md      the change record
```

## Building

```bash
make CROSS=riscv-none-elf
```

That produces `build/firmware.elf`, `.bin`, `.hex` and the Verilog
`$readmemh` images `build/firmware_imem.mem` / `build/firmware_dmem.mem`.
Other targets: `make size`, `make disasm`, `make tests`, `make tests-mem`.

### Build status

Cross-compiles clean with **xPack GNU RISC-V Embedded GCC 15.2.0**
(darwin-arm64) — zero warnings and zero errors across all 20 translation
units and all 17 test images:

```
   text    data     bss    dec    hex
  12140      20    5088  17248   4360   build/firmware.elf
```

IRAM 12,140 B of 64 KB (18.5 %), DRAM 5,108 B of 64 KB (7.8 %). Both of
`soc.ld`'s fixed-address asserts pass, which the link enforces:
`_trap_vector_start` at `0x0001_0000`, `_start` at `0x0001_0080`. Full
numbers, and a size comparison against the NPU build, are in
`docs/NPU_REMOVAL.md` §3 and §7.

Nothing has been run on an Ibex or in a simulator yet, so the latency
figure in the change record is a static estimate from the generated code,
not a measurement.

### The toolchain

Installed on this machine at:

```
/Users/sanjai/opt/xpack-riscv-none-elf-gcc-15.2.0-1/bin
```

xPack GNU RISC-V Embedded GCC 15.2.0, unpacked from the project's
published release and checksum-verified. It needs no Xcode licence and
installs nothing system-wide — which matters here, because `/usr/bin/make`,
`cc` and `brew` on this machine all fail with "You have not agreed to the
Xcode license agreements" until someone runs
`sudo xcodebuild -license accept`. Use the Command Line Tools `make`
(`/Library/Developer/CommandLineTools/usr/bin/make`) meanwhile.

To put the compiler on `PATH` for every new shell, add this to `~/.zshrc`:

```bash
export PATH="$HOME/opt/xpack-riscv-none-elf-gcc-15.2.0-1/bin:$PATH"
```

`CROSS` selects the toolchain prefix: `riscv-none-elf` for this xPack
build, `riscv64-elf` for the Homebrew formula, `riscv64-unknown-elf` for
most others. Fresh copies of the xPack release are at
[xpack-dev-tools/riscv-none-elf-gcc-xpack](https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases).

## Verifying without the cross toolchain

Two targets run on the host compiler alone:

```bash
make host-test
```

```bash
make syntax-check
```

On this machine, where the Xcode licence is unaccepted, `cc` and
`/usr/bin/make` both refuse to run, so pass the Command Line Tools
compiler explicitly (the Makefile locates the macOS SDK on its own, so
nothing else needs exporting):

```bash
/Library/Developer/CommandLineTools/usr/bin/make CC=/Library/Developer/CommandLineTools/usr/bin/clang host-test
```

`host-test` compiles the real `features.c`, `conv1d.c`, `inference.c`,
`snn.c`, `uart.c`, `timer.c`, `intc.c`, `telemetry.c`, `crc16.c` and
`latency.c` against a software device model and checks them against
references written independently. `syntax-check` parses the sources the
harness cannot link (`main.c`, `trap.c`, `sensor.c`, `test/*.c`).

Current result of `make host-test`:

```
T1 conv1d_tile() == unoptimised 8x32*32x8 INT8 matmul
  ok: 500 tiles x 64 accumulators, zero mismatches
  ok: worst-case accumulator 516128 (INT32 max 2147483647)
T2 conv1d_layer1() == textbook same-padded Conv1D(8,k=5)
  ok: 1024 activations identical
T3 conv1d_layer2() == textbook same-padded Conv1D(16,k=3)
  ok: 1024 activations identical
T4 maxpool1d_int8() and global_avg_pool_int8()
  ok: both pooling stages match
T5 inference_run() end to end
  ok: decision NORMAL, reproducible, 2 SNN launches
  ok: latency samples recorded (count 2)
  ok: sensor timeout reported as stage 1
  ok: pipeline recovers after an error
T6 telemetry framing and CRC
  ok: 4 frames, CRC16-Modbus verified independently

PASS: all host checks passed
```

The harness is a software fixture. It is not RTL evidence and it is not
project gate G4 — see `host/README.md` for exactly what it does and does
not establish.

## What this build does not claim

- **No detection accuracy.** The SNN decision rule
  (`popcount(spikes) >= 8`) is an uncalibrated placeholder and the SNN's
  threshold / leak / timestep values are RTL reset values. Project
  blocker BK7. Removing the NPU does not change this either way.
- **Not bit-exactness-gated.** Gate G4 needs Person 2's golden vectors,
  which are not in the repository (blocker BK6). The host harness checks
  the firmware against references written here, which is weaker.
- **Not reconciled with the GF180 signal set.** This firmware acquires a
  3-axis accelerometer window over UART; the ALTair architecture document
  describes a 46-channel motor-current / wheel-speed / inertial telemetry
  frame. That conflict is blocker BK5 and is untouched by this change.
