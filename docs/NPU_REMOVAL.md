# NPU removal — change record

**Baseline:** the Edge-AI SoC firmware in `ALTair-MLFW/firmware.zip`
(SHA-256 `d638fd5e8c96ca10000a85e5fcbf234ed085cf2f02d9608bd9079420d3ed557f`),
which targets an Ibex RV32IM SoC with both an NPU and an SNN.

**This tree:** the same firmware, complete, with the NPU block removed and
the work it did moved onto the CPU. The deployed model, its weights and
its fixed-point contract are unchanged.

---

## 1. Why this is a small change

The NPU was not a network accelerator. Per `START` pulse it computed
exactly one fixed 8×32-by-32×8 INT8 matrix multiply into 64 INT32
accumulators:

```
acc[f*8 + j] = SUM over r = 0..31 of  W[r*8 + f] * A[r*8 + j]
```

`W` was the 256-byte weight tile resident in its weight SRAM, `A` the
256-byte activation tile the DMA wrote into its activation SRAM, `f` the
filter lane and `j` the output-timestep lane. Everything else about the
model already ran in firmware:

| Stage | NPU build | This build |
|---|---|---|
| Input quantization | firmware (`features.c`) | unchanged |
| im2col tiling | firmware (`features.c`) | unchanged |
| The 8×32·32×8 matmul | **NPU, 32 launches** | **CPU (`conv1d.c`)** |
| Bias add | firmware | unchanged |
| ReLU | NPU `relu_en` — *but disabled*, see below | firmware |
| Requantization | firmware | unchanged |
| MaxPool1D | firmware (`features.c`) | unchanged |
| GlobalAveragePooling | firmware (`features.c`) | unchanged |
| Layer sequencing | firmware (`inference.c`) | unchanged |
| SNN hand-off and decision | firmware | unchanged |

The ReLU row needs a note: the deployed configuration launched the NPU
with `NPU_ACT_NONE`, never `NPU_ACT_RELU`, because bias existed only in
firmware and ReLU has to happen *after* the bias add. So the NPU's one
activation function was already unused. Removing the block takes away one
integer matmul and nothing else.

## 2. Numerical equivalence

`conv1d_tile()` computes the same sum, from the same two 256-byte tiles,
into the same 64 INT32 accumulators, in the same index order. The result
is identical, not approximate, for two reasons:

1. **No overflow.** The worst case is `32 × 127 × 127 = 516_128`, about
   4000× inside INT32's range. The products are exact and so is their sum.
2. **Order cannot matter.** Integer addition is associative and
   commutative, so no reassociation the CPU loop performs relative to the
   MAC array's dataflow can change the total.

`conv1d.c` skips rows where the widened weight and activation rows are
both zero, and skips zero weight lanes. Both are algebraic no-ops
(`0 * a = 0`), not rounding shortcuts. The real weight tiles make those
skips fire on real data: layer 1 uses 20 of 32 im2col rows and layer 2
uses 24, so a third of the nominal MAC count is structural zero padding
the MAC array had to clock through regardless.

Consequence for verification: **any tile-level golden vector captured
against the NPU build is still a valid vector for this build.** The tile
sequence is unchanged too — `conv1d_layer2()` keeps the filter-group-outer,
timestep-group-inner order the NPU build used (there chosen to amortise
weight-SRAM reloads; kept here purely so the per-tile vectors line up one
for one).

This is checked two ways, both of which pass:

- `host/test_host.c` T1 — `conv1d_tile()` against an unoptimised
  literal matmul, 500 tiles × 64 accumulators, zero mismatches, mixing
  dense random tiles with the real (zero-padded) weight tiles.
- `host/test_host.c` T2/T3 and `test/test_conv1d.c` — the full layers,
  driven through the real im2col tiling, against a textbook `same`-padded
  Conv1D computed directly from the weight tiles including bias, ReLU and
  requantization. 1024 activations per layer, identical.

## 3. Cost

### Compute

32 tiles per inference, 16 per Conv1D layer, 64 accumulators each, 32
contraction rows:

| | MACs per inference |
|---|---|
| Nominal, all 32 rows | 65_536 |
| Non-padding rows only (20 of 32 in layer 1, 24 in layer 2) | 45_056 |
| **Actually executed**, after the zero-weight-lane skip on the real tiles | **44_160** |

The three real weight tiles have 157, 189 and 187 nonzero entries of 256
respectively, which is where the last reduction comes from.

GCC 15.2.0 at `-O2` compiles the innermost loop to **9 instructions per
MAC** — two loads, `mul`, `add`, a store of the accumulator, two pointer
bumps and the branch. On Ibex RV32IM, where loads and stores take 2
cycles, that is roughly 11–15 cycles per MAC:

| cycles/MAC | cycles | at 50 MHz | share of a 125 ms frame |
|---|---|---|---|
| 11 | 485_760 | 9.7 ms | 7.8 % |
| 13 | 574_080 | 11.5 ms | 9.2 % |
| 15 | 662_400 | 13.2 ms | 10.6 % |

So **about 10–13 ms per inference, using 8–11 % of the frame budget.**

This is a static estimate read off the generated code, **not a
measurement.** The real figure comes from the `PKT_LATENCY` telemetry
packet on hardware or in simulation; `inference.c` keeps the measurement
window identical to the NPU build's precisely so the two are directly
comparable.

> Correction: an earlier draft of this document put the cost in the "low
> hundreds of microseconds". That was wrong by roughly 40×, from
> double-counting tiles per layer and from assuming ~1 cycle per MAC. The
> headroom conclusion is unchanged, but the number above is the right one.

If that budget ever tightens, the cheapest win is visible in the
disassembly: the loop stores each accumulator back to memory on every MAC,
because `acc_out` is a pointer the compiler cannot prove does not alias
the tiles. Inverting the loop nest — filter lane outer, contraction row
inner — would keep 8 accumulators in registers across all 32 rows and
remove that store, for an expected 2–3× saving. It stays bit-identical for
the same reason §2 gives, and `host/test_host.c`'s T1 is exactly the test
that proves it. This was left undone deliberately: the budget does not
need it, and it would trade a verified loop for speed nothing is asking
for.

### Footprint

Both builds compiled with the same toolchain (xPack GNU RISC-V Embedded
GCC 15.2.0, `-O2`, `-march=rv32im_zicsr`):

| Section | NPU build | NPU-free | Δ |
|---|---|---|---|
| `.vectors` | 260 | 260 | — |
| `.text` | 10_908 | 9_972 | **−936** |
| `.rodata` | 2_332 | 1_908 | **−424** |
| `.data` | 20 | 20 | — |
| `.bss` | 4_324 | 5_088 | **+764** |
| IRAM (text+rodata+vectors+data LMA) | 13_500 | **12_140** | **−1_360 (−10.1 %)** |
| DRAM (.data+.bss) | 4_344 | **5_108** | **+764 (+17.6 %)** |

IRAM shrinks by 1.3 KB: the NPU driver, its register documentation
strings and the launch/poll machinery are gone. DRAM grows by 764 B
because `conv1d.c` holds the tile staging buffer and two 64-entry INT32
accumulator arrays as statics, where the NPU build kept accumulators in
the accelerator's own SRAM and staged only one tile.

Against the 64 KB of each: **IRAM 18.5 % used, DRAM 7.8 % used**, leaving
roughly 59 KB of DRAM below the stack top.

### What the hardware no longer does

Removed along with the block: the DMA traffic for 32 tile transfers, 32
launch/poll round trips, the NPU's completion interrupt, and a 256-byte
weight-SRAM reload per launch group.

## 4. Files

**Deleted**

| File | Why |
|---|---|
| `include/npu.h`, `src/npu.c` | the driver for a block that no longer exists |
| `test/test_npu.c` | tested launch/poll |
| `test/test_npu_irq.c` | tested the BUSY-falling-edge interrupt |

**Added**

| File | Why |
|---|---|
| `include/conv1d.h`, `src/conv1d.c` | the software Conv1D engine |
| `test/test_conv1d.c` | on-target replacement for the two NPU tests |
| `host/*` | native harness, so this is testable with no RISC-V toolchain |
| `docs/NPU_REMOVAL.md` | this file |

**Renamed** (contents unchanged unless noted)

| Was | Now |
|---|---|
| `include/npu_weights_layout.h` | `include/model_layout.h` |
| `src/npu_weights_data.c` | `src/model_weights_data.c` |
| `g_npu_weight_l1` / `_l2_g0` / `_l2_g1` | `g_conv_weight_l1` / `_l2_g0` / `_l2_g1` |
| `g_npu_bias_l1` / `_l2` | `g_conv_bias_l1` / `_l2` |
| `NPU_L1_FILTERS` / `NPU_L2_FILTERS` | `L1_FILTERS` / `L2_FILTERS` |
| `NPU_GAP_FEATURES` | `GAP_FEATURES` |
| `NPU_IMAGE_BYTES` | `TILE_BYTES` |
| `NPU_ROWS` / `NPU_LANES` | `TILE_ROWS` / `TILE_LANES` |
| `NPU_INPUT_SHIFT` | `INPUT_QUANT_SHIFT` |
| `NPU_L1_REQUANT_SHIFT` / `_L2_` | `L1_REQUANT_SHIFT` / `L2_` |
| `L1_LAUNCHES` / `L2_LAUNCHES` | `L1_TILES` / `L2_TILES` |
| `DMA_CH_MEM_TO_NPU` / `DMA_CH_NPU_TO_MEM` | `DMA_CH_GP0` / `DMA_CH_GP1` |

The numeric values behind every renamed macro are unchanged. They are the
model's quantization contract and must stay bit-identical to
`npu_model.py`; they were never properties of the hardware that executed
them.

**Edited**

- `include/soc.h` — `NPU_BASE` removed; `0x2000_0000` documented as
  deliberately unmapped so a stray access raises a bus error instead of
  aliasing a live peripheral. Also gained the `FW_HOST_BUILD` hook.
- `include/intc.h` — `IRQ_NPU` removed. INTC input 6 is tied low and
  cannot become pending; the constant is gone rather than left maskable.
  The priority word is positional, so bit 6 must not be reused without
  updating that packing.
- `include/dma.h` — CH0/CH1 renamed. Their old NPU-destination routing is
  unreachable, but the channels are untouched RTL and still work as
  general mem↔mem movers (routing is by destination address, not channel
  number), so they were kept and renamed rather than deleted.
- `src/trap.c` — `npu_irq_service()`, `_npu_irq_done` and the `IRQ_NPU`
  dispatch arm removed.
- `src/inference.c` / `include/inference.h` — rewritten around
  `conv1d_layer1()` / `conv1d_layer2()`. See §5 for the error codes.
- `src/main.c` — the `npu_init()` step is gone and the init steps are
  renumbered. The `[dbg-pipe]` UART breadcrumbs were also removed: they
  existed to bisect which DMA or NPU launch was hanging, and every site
  they bracketed is now a pure function that cannot hang. The separate
  trap ring-buffer dump (`crt0.S` capture + `dbg_trap_dump_pending()`) is
  kept — that belongs to the open illegal-instruction investigation and
  has nothing to do with the NPU.
- `telemetry/telemetry.h` — `ERR_NPU_TIMEOUT` (0xE1) retired as
  `ERR_NPU_TIMEOUT_RETIRED`, left defined and reserved rather than reused,
  so a decoder that still knows the old code cannot silently misread a new
  one.
- `include/timer.h`, `src/intc.c` — CSR access (`mcycle`, `mstatus.MIE`,
  `mie.MEIE`) guarded for `FW_HOST_BUILD`. Target behaviour unchanged.
- `Makefile` — `npu.c` out, `conv1d.c` in, `model_*` names, plus
  `host-test` and `syntax-check` targets. Also fixed a pre-existing bug
  unrelated to the NPU: `-Wl,-Map=build/firmware.map` was hardcoded in
  `LDFLAGS`, which the 17 test-image rules inherit, so every test link
  overwrote it and after `make tests` that file described whichever test
  linked last, with no trace of `main.o`. Each link now writes its own
  map (`$(@:.elf=.map)`), giving 18 maps for 18 ELFs. Linker output is
  otherwise byte-identical; only the map filename changed.
- `host/Makefile` — also fixed a portability bug of its own: it relied on
  `SDKROOT` being exported by the caller to give clang a macOS sysroot,
  and failed with "'stdio.h' file not found" for anyone who did not have
  it set. It now locates the SDK itself (Command Line Tools first, then
  Xcode), and `syntax-check` shares the same `CFLAGS` instead of
  duplicating the flag list.

## 5. Error codes: retired, not renumbered

`g_inference_error_stage` was `1=sensor, 2=L1 DMA, 3=L1 NPU, 4=L2 DMA,
5=L2 NPU, 6=SNN`. Stages 2–5 no longer have hardware to wait on —
`conv1d_tile()` is a pure function with no MMIO, no completion poll and
therefore no timeout. Those four codes are **retired, and the surviving
codes keep their old values**:

| Code | Meaning |
|---|---|
| 1 | sensor window acquisition timed out |
| 2–5 | **retired** (were L1/L2 DMA and NPU) |
| 6 | SNN never asserted DONE |

So an existing telemetry decoder stays correct without modification, and a
decoder that sees 2–5 is reading output from an older, NPU-equipped build.
This also means an inference now has exactly two failure modes instead of
about 65.

## 6. Address map

| Range | NPU build | This build |
|---|---|---|
| `0x2000_0000`–`0x2000_FFFF` | NPU registers + weight/activation/accumulator SRAM | **unmapped** |
| `0x2100_0000` | SNN | unchanged |
| `0x2200_0000` | DMA | unchanged |

Against the ALTair GF180 architecture document's logical memory map, the
removal of `0x2000_0000` is a **convergence**: that document specifies the
SNN as the only accelerator and lists no NPU. Deltas that remain, and are
deliberately left alone because this firmware targets the RTL as actually
built:

- this SoC has DMA (`0x2200_0000`), I2C (`0x4200_0000`) and PWM
  (`0x4400_0000`), none of which appear in the GF180 map;
- the GF180 map's telemetry/packet-integrity counters (`0x2300_0000`) and
  boot state / system identity (`0x2400_0000`) blocks do not exist in this
  RTL, so no driver addresses them;
- IRAM here is at `0x0001_0000` with a boot ROM at `0x0`, where the GF180
  document puts instruction SRAM at `0x0000_0000`.

## 7. Build result

Cross-compiled with xPack GNU RISC-V Embedded GCC 15.2.0 (darwin-arm64),
`-march=rv32im_zicsr -mabi=ilp32 -O2 -Wall -Wextra`:

```
make CROSS=riscv-none-elf              # firmware + .bin/.hex/.mem
make CROSS=riscv-none-elf tests        # 17 test images
make CROSS=riscv-none-elf tests-mem    # their .mem images
```

- **Zero warnings and zero errors** across all 20 translation units and
  all 17 test images.
- `soc.ld`'s two fixed-address `ASSERT`s pass, which the link enforces:
  `_trap_vector_start` lands at `0x0001_0000` (IRAM+0x0, where the boot
  ROM's vectored table jumps) and `_start` at `0x0001_0080` (IRAM+0x80,
  the boot ROM's hardwired trampoline target). Confirmed in the symbol
  table, and the first instruction at `0x10080` is the stack-pointer
  init `auipc sp,0x10000 / addi sp,sp,-128`, as `crt0.S` intends.
- `build/firmware.elf`, `.bin`, `.hex`, `firmware_imem.mem` and
  `firmware_dmem.mem` all produced.

Not yet done: nothing has been run on an Ibex or in a simulator. See §8.

## 8. Open items this change does not address

These were open before the NPU removal and are open after it. None of them
is made better or worse by it.

- **BK7 — the decision rule is uncalibrated.** `popcount(spikes) >= 8` is
  a placeholder, and the SNN threshold / leak / timestep values this
  firmware programs are RTL reset values. No detection accuracy can be
  attached to this build.
- **BK6 — no bit-exact golden reference in the repository.**
  `npu_model.py`, named by `features.c` and `model_layout.h` as the
  reference that generated the RTL test vectors, is not present. Gate G4
  cannot be closed by this tree; §2's checks are against references
  written locally, which is weaker.
- **BK5 — signal-set conflict.** This firmware acquires a 3-axis
  accelerometer window (milli-g, 128 samples, over UART); the
  architecture document describes a 46-channel motor-current / wheel-speed
  / inertial frame reduced to 32 INT8 features.
- **BK4 — the GF180 64-48-16-48-32 spiking autoencoder is not what this
  SNN RTL implements.** The RTL's synapse memory stores one scalar INT8
  weight per neuron, not a weight matrix.
- **Telemetry CRC mismatch.** This firmware's outgoing frames use
  CRC16-Modbus (poly `0xA001` reflected, init `0xFFFF`); the architecture
  document specifies CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, not
  reflected) for the telemetry wire format. Changing it is a telemetry
  contract decision, not part of removing the NPU, so the existing format
  was left as it is.
- **Not run on hardware or in simulation.** The firmware and all 17 test
  images cross-compile clean (see §8), but nothing here has executed on
  an Ibex, so the latency figure in §3 is an estimate and the peripheral
  test images' PASS/FAIL lines have not been observed.
