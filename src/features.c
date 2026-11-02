/*
 * features.c -- tensor assembly, im2col tiling and software pooling for
 * the Conv1D front-end. No stdlib / POSIX headers. No float.
 *
 * Bit-exact C port of tinyml_training/tinyml_training/npu_model.py -- see
 * features.h. Unchanged arithmetic in the NPU-free build; the activation
 * tiles built here are now consumed by src/conv1d.c on the CPU instead of
 * being DMA'd into the NPU's activation SRAM.
 */

#include "features.h"

/* isqrt_u64() - floor(sqrt(n)) via Newton's method, integer-only.
 * Mirrors npu_model.isqrt_i64() exactly (same algorithm, same truncating
 * unsigned division). */
static uint64_t isqrt_u64(uint64_t n)
{
    uint64_t x, y;

    if (n == 0U) {
        return 0U;
    }
    x = n;
    y = (x + 1U) / 2U;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2U;
    }
    return x;
}

/* round_shift_i32() - round-to-nearest signed right shift.
 * Sign-magnitude rounding (not C's implementation-defined '>>' on
 * negatives) — mirrors npu_model.round_shift() exactly. */
static int32_t round_shift_i32(int32_t v, int32_t shift)
{
    int32_t half, mag;

    if (shift <= 0) {
        return v;
    }
    half = (int32_t)1 << (shift - 1);
    if (v >= 0) {
        return (v + half) >> shift;
    }
    mag = ((-v) + half) >> shift;
    return -mag;
}

/* clamp_i8_sym() - clamp to the symmetric int8 range [-127, 127].
 * Mirrors npu_model.clamp_i8_sym(). */
static int8_t clamp_i8_sym(int32_t v)
{
    if (v > 127) {
        return 127;
    }
    if (v < -127) {
        return -127;
    }
    return (int8_t)v;
}

void tensor_build_and_quantize(const int16_t *x, const int16_t *y,
                                const int16_t *z,
                                int8_t tensor_out[SENSOR_WINDOW_LEN][N_CHANNELS_IN])
{
    uint32_t t;

    for (t = 0U; t < SENSOR_WINDOW_LEN; t++) {
        int64_t xi = (int64_t)x[t];
        int64_t yi = (int64_t)y[t];
        int64_t zi = (int64_t)z[t];
        uint64_t sum_sq = (uint64_t)(xi * xi + yi * yi + zi * zi);
        int32_t mag = (int32_t)isqrt_u64(sum_sq);

        tensor_out[t][0] = clamp_i8_sym(round_shift_i32((int32_t)x[t], INPUT_QUANT_SHIFT));
        tensor_out[t][1] = clamp_i8_sym(round_shift_i32((int32_t)y[t], INPUT_QUANT_SHIFT));
        tensor_out[t][2] = clamp_i8_sym(round_shift_i32((int32_t)z[t], INPUT_QUANT_SHIFT));
        tensor_out[t][3] = clamp_i8_sym(round_shift_i32(mag, INPUT_QUANT_SHIFT));
    }
}

/* im2col_patch_l1() - 'same'-padding tap for layer 1 (kernel=5): relative
 * timestep offset k - L1_KERNEL/2. Out-of-range taps are zero. */
static int8_t im2col_patch_l1(const int8_t tensor[SENSOR_WINDOW_LEN][N_CHANNELS_IN],
                              int32_t t, uint32_t k, uint32_t c)
{
    int32_t src_t = t + (int32_t)k - (int32_t)(L1_KERNEL / 2U);

    if (src_t >= 0 && src_t < (int32_t)SENSOR_WINDOW_LEN) {
        return tensor[src_t][c];
    }
    return 0;
}

void build_l1_activation_image(const int8_t tensor[SENSOR_WINDOW_LEN][N_CHANNELS_IN],
                                uint32_t group,
                                uint8_t image_out[TILE_BYTES])
{
    uint32_t k, c, j, i;

    for (i = 0U; i < TILE_BYTES; i++) {
        image_out[i] = 0U;
    }
    for (k = 0U; k < L1_KERNEL; k++) {
        for (c = 0U; c < N_CHANNELS_IN; c++) {
            uint32_t r = k * N_CHANNELS_IN + c;
            for (j = 0U; j < TILE_LANES; j++) {
                int32_t t = (int32_t)(group * TILE_LANES + j);
                image_out[r * 8U + j] = (uint8_t)im2col_patch_l1(tensor, t, k, c);
            }
        }
    }
}

void maxpool1d_int8(const int8_t in[SENSOR_WINDOW_LEN][L1_FILTERS],
                     int8_t out[POOL_LEN][L2_IN_CHANNELS])
{
    uint32_t t, c, p;

    for (t = 0U; t < POOL_LEN; t++) {
        for (c = 0U; c < L2_IN_CHANNELS; c++) {
            int8_t m = in[t * POOL_SIZE][c];
            for (p = 1U; p < POOL_SIZE; p++) {
                int8_t v = in[t * POOL_SIZE + p][c];
                if (v > m) {
                    m = v;
                }
            }
            out[t][c] = m;
        }
    }
}

/* im2col_patch_l2() - 'same'-padding tap for layer 2 (kernel=3): relative
 * timestep offset k - L2_KERNEL/2. Out-of-range taps are zero. */
static int8_t im2col_patch_l2(const int8_t pooled[POOL_LEN][L2_IN_CHANNELS],
                              int32_t t, uint32_t k, uint32_t c)
{
    int32_t src_t = t + (int32_t)k - (int32_t)(L2_KERNEL / 2U);

    if (src_t >= 0 && src_t < (int32_t)POOL_LEN) {
        return pooled[src_t][c];
    }
    return 0;
}

void build_l2_activation_image(const int8_t pooled[POOL_LEN][L2_IN_CHANNELS],
                                uint32_t timestep_group,
                                uint8_t image_out[TILE_BYTES])
{
    uint32_t k, c, j, i;

    for (i = 0U; i < TILE_BYTES; i++) {
        image_out[i] = 0U;
    }
    for (k = 0U; k < L2_KERNEL; k++) {
        for (c = 0U; c < L2_IN_CHANNELS; c++) {
            uint32_t r = k * L2_IN_CHANNELS + c;
            for (j = 0U; j < TILE_LANES; j++) {
                int32_t t = (int32_t)(timestep_group * TILE_LANES + j);
                image_out[r * 8U + j] = (uint8_t)im2col_patch_l2(pooled, t, k, c);
            }
        }
    }
}

void global_avg_pool_int8(const int8_t in[POOL_LEN][L2_FILTERS],
                           int8_t out[L2_FILTERS])
{
    uint32_t c, t;
    /* POOL_LEN (64) is a power of two -- shift 6, exact, matches
     * npu_model.global_avg_pool()'s t_len.bit_length()-1 computation. */
    const int32_t shift = 6;

    for (c = 0U; c < L2_FILTERS; c++) {
        int32_t total = 0;
        for (t = 0U; t < POOL_LEN; t++) {
            total += (int32_t)in[t][c];
        }
        out[c] = clamp_i8_sym(round_shift_i32(total, shift));
    }
}
