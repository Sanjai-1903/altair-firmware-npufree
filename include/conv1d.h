#ifndef CONV1D_H
#define CONV1D_H

/*
 * conv1d.h — software Conv1D engine. This is the block that replaces the
 * NPU.
 *
 * WHAT THE NPU USED TO DO
 * -----------------------
 * The NPU was not a network accelerator. Per launch it computed exactly
 * one fixed 8x32-times-32x8 INT8 matmul into 64 INT32 accumulators:
 *
 *     acc[f * 8 + j] = SUM over r = 0..31 of  W[r * 8 + f] * A[r * 8 + j]
 *
 * with W the 256-byte weight tile resident in its weight SRAM, A the
 * 256-byte activation tile DMA'd into its activation SRAM, f the filter
 * lane and j the output-timestep lane (mac_array.sv's array_out[i*8+j]).
 * Everything else -- im2col tiling, the bias add, the ReLU in the deployed
 * configuration, requantization, pooling and layer sequencing -- was
 * already firmware. A Conv1D layer was decomposed onto that primitive by
 * firmware as 16 launches.
 *
 * WHAT THIS DOES INSTEAD
 * ----------------------
 * conv1d_tile() computes that same sum on the CPU, from the same two
 * 256-byte tiles, into the same 64 INT32 accumulators in the same index
 * order. The result is not an approximation of the NPU's output: INT8 x
 * INT8 products accumulated in INT32 are exact (|acc| <= 32 * 127 * 127 =
 * 516_128, three orders of magnitude inside INT32), and integer addition
 * is associative, so summation order cannot change the result either.
 * Any tile-level golden vector captured against the NPU build is
 * therefore still a valid vector for this build.
 *
 * COST
 * ----
 * The deployed model is 32 tiles per inference, 16 per Conv1D layer, 64
 * accumulators each, 32 contraction rows: 65_536 MACs nominal. Only
 * 45_056 of those rows carry real data (layer 1 uses 20 of 32 im2col
 * rows, layer 2 uses 24; the rest is structural zero padding), and with
 * the real weight tiles the zero-weight-lane skip below brings the MACs
 * actually executed to 44_160 per inference.
 *
 * GCC 15.2.0 at -O2 compiles the innermost loop to 9 instructions per MAC
 * (two loads, mul, add, a store of the accumulator, two pointer bumps and
 * the branch), so on Ibex RV32IM -- where loads and stores cost 2 cycles
 * -- expect roughly 11 to 15 cycles per MAC:
 *
 *     44_160 MACs  ->  ~0.49M to 0.66M cycles  ->  ~10 to 13 ms at 50 MHz
 *
 * The ALTair architecture document asks for one inference per 125 ms
 * frame, so this uses about 8-11% of the frame budget. That is a static
 * estimate from the generated code, NOT a measurement; the real figure
 * comes from the PKT_LATENCY telemetry packet on hardware or in
 * simulation. See docs/NPU_REMOVAL.md section 3.
 *
 * If that budget ever tightens, the cheapest win is visible right in the
 * disassembly: the loop stores each accumulator back to memory on every
 * MAC, because acc_out is a pointer the compiler cannot prove does not
 * alias the tiles. Making the row loop inner and the filter lane outer
 * would keep 8 accumulators in registers across all 32 rows and remove
 * that store. It stays bit-identical for the same reason the rest of this
 * does -- integer addition reassociates freely -- and host/test_host.c's
 * T1 is exactly the test that would prove it.
 *
 * WHAT IS GONE
 * ------------
 * No MMIO, no DMA, no launch, no completion poll, no interrupt, and so no
 * timeout: conv1d_tile() cannot fail. That is why the inference pipeline's
 * error stages 2..5 (L1 DMA, L1 NPU, L2 DMA, L2 NPU) no longer exist --
 * see inference.h.
 */

#include <stdint.h>
#include "model_layout.h"
#include "features.h"
#include "sensor.h"

/* Number of accumulators one tile produces: 8 filter lanes x 8 timestep
 * lanes. Indexed acc[filter_lane * TILE_LANES + timestep_lane], the same
 * index the NPU's accumulator window used. */
#define CONV1D_TILE_ACCS   (TILE_LANES * TILE_LANES)   /* 64 */

/**
 * conv1d_tile() - the one-launch-equivalent INT8 matmul.
 * @weight_tile: TILE_BYTES bytes, tile[row * TILE_LANES + filter_lane],
 *               signed INT8 values stored as unsigned bytes.
 * @act_tile:    TILE_BYTES bytes, tile[row * TILE_LANES + timestep_lane],
 *               as produced by build_l{1,2}_activation_image().
 * @acc_out:     CONV1D_TILE_ACCS INT32 accumulators, overwritten.
 *
 * Pure function of its inputs. No bias, no activation, no requantization
 * -- exactly like the NPU launch it replaces in the deployed
 * configuration, where ACTIVATION_CFG was NPU_ACT_NONE because the bias
 * add lives in firmware and ReLU must happen after it.
 */
void conv1d_tile(const uint8_t *weight_tile,
                 const uint8_t *act_tile,
                 int32_t acc_out[CONV1D_TILE_ACCS]);

/**
 * conv1d_layer1() - Conv1D(8, k=5, same) + bias + ReLU + requantize over
 * a whole window.
 * @tensor: [128][4] quantized input tensor from tensor_build_and_quantize().
 * @out:    [128][8] INT8 layer-1 activations.
 *
 * Walks L1_TILES tiles, in the same order the NPU build launched them.
 */
void conv1d_layer1(const int8_t tensor[SENSOR_WINDOW_LEN][N_CHANNELS_IN],
                   int8_t out[SENSOR_WINDOW_LEN][L1_FILTERS]);

/**
 * conv1d_layer2() - Conv1D(16, k=3, same) + bias + ReLU + requantize over
 * the pooled tensor.
 * @pooled: [64][8] from maxpool1d_int8().
 * @out:    [64][16] INT8 layer-2 activations.
 *
 * Walks L2_FILTER_GROUPS x L2_TIMESTEP_GROUPS tiles, in the same order the
 * NPU build launched them (filter group outer, timestep group inner --
 * which is why the NPU build reloaded its weight SRAM only twice).
 */
void conv1d_layer2(const int8_t pooled[POOL_LEN][L2_IN_CHANNELS],
                   int8_t out[POOL_LEN][L2_FILTERS]);

#endif /* CONV1D_H */
