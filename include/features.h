#ifndef FEATURES_H
#define FEATURES_H

/*
 * features.h — tensor assembly, im2col tiling and the software pooling
 * stages of the Conv1D front-end.
 *
 * This is a bit-exact C port of tinyml_training/tinyml_training/
 * npu_model.py's build_input_tensor() / build_l1_activation_image() /
 * maxpool1d() / build_l2_activation_image() / global_avg_pool() -- that
 * Python module is the reference used to generate the RTL test vectors, so
 * any change here must be mirrored there (and vice versa) or the cocotb
 * regression stops proving anything.
 *
 * NPU-FREE BUILD. This file is byte-for-byte the same arithmetic as in the
 * NPU build; only its comments and macro names changed. The activation
 * tiles it builds used to be DMA'd into the NPU's activation SRAM and are
 * now consumed directly by src/conv1d.c on the CPU. They are still built
 * tile by tile, in the same order and the same layout, so that the
 * existing per-launch golden vectors remain valid as per-tile golden
 * vectors.
 *
 * Pipeline (orchestration in src/inference.c):
 *   sensor_read_window() -> tensor_build_and_quantize() [128x4 int8] ->
 *   16x (build_l1_activation_image() -> conv1d_tile()) -> maxpool1d_int8()
 *   [64x8 int8] -> 16x (build_l2_activation_image() -> conv1d_tile()) ->
 *   global_avg_pool_int8() [16 int8] -> snn_write_input() x16 -> SNN.
 *
 * No statistics (mean/std) are computed anywhere in this file -- the CNN
 * consumes the raw {x, y, z, magnitude} tensor directly, per the approved
 * architecture (Conv1D operates on the full window, not per-window summary
 * statistics).
 */

#include <stdint.h>
#include "sensor.h"
#include "model_layout.h"   /* L1_FILTERS, L2_FILTERS, TILE_*, shifts */

#define N_CHANNELS_IN     4U      /* x, y, z, magnitude */
#define L1_DEPTH          (L1_KERNEL * N_CHANNELS_IN)          /* 20 */
#define POOL_SIZE         2U
#define POOL_LEN          (SENSOR_WINDOW_LEN / POOL_SIZE)       /* 64 */
#define L2_IN_CHANNELS    L1_FILTERS                            /* 8 */
#define L2_DEPTH          (L2_KERNEL * L2_IN_CHANNELS)          /* 24 */

/* Tile counts. Named *_TILES (they were *_LAUNCHES when each one was an
 * NPU launch); the counts themselves are unchanged -- 16 tiles per layer. */
#define L1_TILES           (SENSOR_WINDOW_LEN / TILE_LANES)        /* 16 */
#define L2_FILTER_GROUPS   ((L2_FILTERS + TILE_LANES - 1U) / TILE_LANES)  /* 2 */
#define L2_TIMESTEP_GROUPS (POOL_LEN / TILE_LANES)                 /* 8 */
#define L2_TILES           (L2_FILTER_GROUPS * L2_TIMESTEP_GROUPS) /* 16 */

/**
 * tensor_build_and_quantize() - Build the [128][4] int8 CNN input tensor
 * {x, y, z, magnitude} from one raw sensor window, quantized via the
 * shared power-of-two INPUT_QUANT_SHIFT (from model_layout.h).
 */
void tensor_build_and_quantize(const int16_t *x, const int16_t *y,
                                const int16_t *z,
                                int8_t tensor_out[SENSOR_WINDOW_LEN][N_CHANNELS_IN]);

/**
 * build_l1_activation_image() - im2col-slice one of L1_TILES 256-byte
 * activation tiles from the input tensor, covering output timesteps
 * [group*8, group*8+8). Rows 0..19 = (kernel_pos*4 + channel) im2col
 * depth index, lanes 0..7 = the 8 output timesteps in this group. Rows
 * 20..31 are zero padding.
 */
void build_l1_activation_image(const int8_t tensor[SENSOR_WINDOW_LEN][N_CHANNELS_IN],
                                uint32_t group,
                                uint8_t image_out[TILE_BYTES]);

/**
 * maxpool1d_int8() - Software MaxPool1D(pool_size=2) over the [128][8]
 * Conv1D layer-1 output, producing [64][8]. Pure integer max.
 */
void maxpool1d_int8(const int8_t in[SENSOR_WINDOW_LEN][L1_FILTERS],
                     int8_t out[POOL_LEN][L2_IN_CHANNELS]);

/**
 * build_l2_activation_image() - im2col-slice one of L2_TIMESTEP_GROUPS
 * 256-byte activation tiles from the pooled [64][8] tensor, covering
 * output timesteps [timestep_group*8, timestep_group*8+8). Rows 0..23 =
 * (kernel_pos*8 + channel) im2col depth index. Reused across both
 * L2_FILTER_GROUPS weight tiles (the im2col patch doesn't depend on which
 * filter group is being computed).
 */
void build_l2_activation_image(const int8_t pooled[POOL_LEN][L2_IN_CHANNELS],
                                uint32_t timestep_group,
                                uint8_t image_out[TILE_BYTES]);

/**
 * global_avg_pool_int8() - Mean over the 64-timestep axis of the [64][16]
 * Conv1D layer-2 output, producing 16 int8 values (POOL_LEN is a power of
 * two, so this is an exact right-shift, not a lossy division). This is
 * the deployed feature vector handed to the SNN via snn_write_input().
 */
void global_avg_pool_int8(const int8_t in[POOL_LEN][L2_FILTERS],
                           int8_t out[L2_FILTERS]);

#endif /* FEATURES_H */
