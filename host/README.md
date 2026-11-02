# Host harness

Builds the firmware's integer pipeline with the **host** compiler and runs
it against a software device model, so the NPU-free build can be compiled
and tested with no RISC-V toolchain and no simulator.

```bash
make run            # from here, or `make host-test` from the firmware root
make syntax-check   # parse the sources this harness cannot link
```

On macOS the Makefile finds the SDK itself (`-isysroot`), including when
the Xcode licence is unaccepted and every `xcrun`-backed tool refuses to
run — an `SDKROOT` in the environment or on the command line overrides it.
Where `cc` itself is licence-blocked, pass a working compiler:
`make CC=/Library/Developer/CommandLineTools/usr/bin/clang run`.

## What is real and what is a fixture

**Real firmware source, compiled unmodified:** `conv1d.c`, `features.c`,
`inference.c`, `model_weights_data.c`, `latency.c`, `snn.c`, `uart.c`,
`intc.c`, `timer.c`, `dma.c`, `gpio.c`, `spi.c`, `i2c.c`, `pwm.c`,
`telemetry.c`, `crc16.c`.

**Fixture:**

- `mock_soc.c` — the device model behind `REG()` and `SOC_BYTE_PORT()`
  when `FW_HOST_BUILD` is defined. A flat page per peripheral, plus the
  minimum behaviour that keeps the real drivers running: UART TX is never
  busy and every byte is captured, the timer's `VALUE` advances on each
  read so delay loops terminate, and writing `SNN_CONTROL.START` runs a
  LIF stand-in. It also **aborts on any access to an unmapped address**,
  which is how the tests confirm nothing touches `0x2000_0000`, the window
  the removed NPU occupied.
- `mock_sensor.c` — replaces `src/sensor.c`. `sensor.h` already names the
  byte source as the single layer to swap for real accelerometer
  bring-up; this is that swap, serving windows from the test runner
  instead of UART RX bytes under interrupt.
- `test_host.c` — the checks, with references written independently of the
  firmware code they check.

**Not linked:** `main.c` (its inference loop never returns), `crt0.S` and
`trap.c` (RV32 startup and the machine-mode trap handler have no host
equivalent — `mock_soc.c` defines the ISR flag symbols the drivers expect,
and they stay zero, so every driver takes its polling path). `make
syntax-check` parses these instead.

## What the checks establish

| | |
|---|---|
| T1 | `conv1d_tile()` equals an unoptimised literal 8×32-by-32×8 INT8 matmul over 500 tiles × 64 accumulators — the zero-row and zero-weight skips in `conv1d.c` are exact. This is the core claim of the NPU removal. |
| T1 | The worst-case accumulator is 516_128, which is the INT32 headroom argument made concrete. |
| T2, T3 | Both Conv1D layers, driven through the real im2col tiling, equal a textbook `same`-padded convolution computed directly from the weight tiles, including bias, ReLU and requantization. The tiling decomposition is therefore correct. |
| T4 | `maxpool1d_int8()` and `global_avg_pool_int8()` match independent references. |
| T5 | `inference_run()` completes end to end, is deterministic for a given window, reports the sensor-timeout error path as stage 1, and recovers afterwards. |
| T6 | Telemetry frames decode, with the CRC recomputed by a table-driven implementation independent of `crc16.c`'s bit-by-bit loop. |

## What they do not establish

- **Nothing about the RTL.** `mock_soc.c` is not a model of the hardware.
  Where the real RTL's timing or semantics are subtle, this model is
  simply wrong, on purpose.
- **Nothing about the SNN.** The LIF in `snn_run()` is a stand-in that
  matches the shape of the register contract in `snn.h`, not its
  behaviour. It exists so `inference_run()` has a device that completes.
- **Nothing about timing.** `fw_mock_cycles()` returns a synthetic
  monotonic count. A host build cannot measure the target's cycles.
- **Nothing about detection accuracy.** The decision threshold is
  uncalibrated (blocker BK7).
- **This is not gate G4.** Bit-exactness needs Person 2's golden vectors,
  which are not in the repository (blocker BK6). Checking firmware against
  references written in the same tree is weaker than checking it against
  an independently produced golden package, and should not be reported as
  if it were the same thing.

## One delicate implementation detail

`REG()` expands to a plain dereference, so the device model cannot observe
a store — it only hands out a pointer. It therefore snapshots the word it
is about to hand out and, on the next access, compares the stored word
against that snapshot; a difference means the firmware wrote something.
After servicing a write-triggered register the handler parks a sentinel
value in it, so a second identical write (two `l`s in a row through
`UART_TX`, two back-to-back `START`s) is still detected. `mock_soc_sync()`
forces that comparison and the UART capture accessors call it, so the last
byte written before an inspection is never missed.

This is the one genuinely fragile thing here. It is reliable for the
write-then-other-access patterns every driver in `src/` uses, and it is
why the tests assert on captured output and returned values rather than on
register contents.
