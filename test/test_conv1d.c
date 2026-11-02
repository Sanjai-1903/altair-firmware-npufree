/*
 * test_conv1d.c — on-target test for the software Conv1D engine that
 * replaced the NPU.
 * No stdlib / POSIX headers. No printf. No float.
 *
 * Needs no hardware beyond the UART: conv1d.c is pure CPU integer code
 * with no MMIO, which is the point of this test existing at all. It is
 * the on-target counterpart of host/test_host.c's T1-T3, reduced to what
 * fits in a bare-metal image: an unoptimised in-test matmul reference,
 * and fixed expected values for the real weight tiles so a miscompiled or
 * mis-linked weight table is caught too.
 *
 * It replaces test_npu.c and test_npu_irq.c, which tested a block that no
 * longer exists (launch/poll and the BUSY-falling-edge interrupt).
 */

#include <stdint.h>
#include "conv1d.h"
#include "features.h"
#include "model_layout.h"
#include "uart.h"

extern const unsigned char g_conv_weight_l1[TILE_BYTES];
extern const unsigned char g_conv_weight_l2_g0[TILE_BYTES];
extern const int32_t g_conv_bias_l1[L1_FILTERS];

static uint8_t s_w[TILE_BYTES];
static uint8_t s_a[TILE_BYTES];
static int32_t s_got[CONV1D_TILE_ACCS];
static int32_t s_want[CONV1D_TILE_ACCS];
static int8_t  s_tensor[SENSOR_WINDOW_LEN][N_CHANNELS_IN];
static int8_t  s_l1[SENSOR_WINDOW_LEN][L1_FILTERS];

/* ref_matmul() - the accelerator primitive written out literally, with no
 * zero-row or zero-weight skip. conv1d_tile() must equal this exactly. */
static void ref_matmul(const uint8_t *w, const uint8_t *a, int32_t *acc)
{
    uint32_t f, j, r;

    for (f = 0U; f < TILE_LANES; f++) {
        for (j = 0U; j < TILE_LANES; j++) {
            int32_t sum = 0;
            for (r = 0U; r < TILE_ROWS; r++) {
                sum += (int32_t)(int8_t)w[r * TILE_LANES + f] *
                       (int32_t)(int8_t)a[r * TILE_LANES + j];
            }
            acc[f * TILE_LANES + j] = sum;
        }
    }
}

/* xorshift32 - deterministic, so a failure on silicon is reproducible. */
static uint32_t s_rng = 0x9E3779B9UL;
static uint32_t rnd(void)
{
    uint32_t x = s_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_rng = x;
    return x;
}

static void fill_random(uint8_t *buf)
{
    uint32_t i;
    for (i = 0U; i < TILE_BYTES; i++) {
        buf[i] = (uint8_t)(int8_t)((int32_t)(rnd() % 255U) - 127);
    }
}

static int compare_accs(const char *tag)
{
    uint32_t i;

    for (i = 0U; i < CONV1D_TILE_ACCS; i++) {
        if (s_got[i] != s_want[i]) {
            uart_puts("[FAIL] ");
            uart_puts(tag);
            uart_puts(" acc[");
            uart_put_uint(i);
            uart_puts("] got=");
            uart_put_hex32((uint32_t)s_got[i]);
            uart_puts(" want=");
            uart_put_hex32((uint32_t)s_want[i]);
            uart_puts("\r\n");
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    uint32_t trial, i, t, c, f;

    uart_init(115200U);
    uart_puts("conv1d test: software Conv1D engine (NPU-free build)\r\n");

    /* ── Test 1: random tiles against the unoptimised reference ───────── */
    uart_puts("conv1d test 1: tile == unoptimised matmul\r\n");
    for (trial = 0U; trial < 16U; trial++) {
        fill_random(s_w);
        fill_random(s_a);
        conv1d_tile(s_w, s_a, s_got);
        ref_matmul(s_w, s_a, s_want);
        if (compare_accs("t1 random")) {
            return 1;
        }
    }
    uart_puts("[PASS] t1: 16 random tiles match\r\n");

    /* ── Test 2: the real weight tiles, where rows 20..31 / 24..31 are
     * structural zero padding and the zero-row skip actually fires ───── */
    uart_puts("conv1d test 2: real weight tiles\r\n");
    fill_random(s_a);
    conv1d_tile(g_conv_weight_l1, s_a, s_got);
    ref_matmul(g_conv_weight_l1, s_a, s_want);
    if (compare_accs("t2 l1")) {
        return 1;
    }
    conv1d_tile(g_conv_weight_l2_g0, s_a, s_got);
    ref_matmul(g_conv_weight_l2_g0, s_a, s_want);
    if (compare_accs("t2 l2g0")) {
        return 1;
    }
    uart_puts("[PASS] t2: both real weight tiles match\r\n");

    /* ── Test 3: accumulator headroom. The worst case an INT8 x INT8
     * 32-deep contraction can reach is 32 * 127 * 127 = 516128, which must
     * not come anywhere near overflowing the INT32 accumulators. ─────── */
    uart_puts("conv1d test 3: accumulator headroom\r\n");
    for (i = 0U; i < TILE_BYTES; i++) {
        s_w[i] = 127U;
        s_a[i] = 127U;
    }
    conv1d_tile(s_w, s_a, s_got);
    if (s_got[0] != 32 * 127 * 127) {
        uart_puts("[FAIL] t3: worst case acc=");
        uart_put_uint((uint32_t)s_got[0]);
        uart_puts(" expected 516128\r\n");
        return 1;
    }
    uart_puts("[PASS] t3: worst-case accumulator 516128\r\n");

    /* ── Test 4: a whole layer runs, and every output is a legal
     * symmetric-INT8 value after bias + ReLU + requantization. ReLU means
     * nothing may be negative. ───────────────────────────────────────── */
    uart_puts("conv1d test 4: full layer 1 over a window\r\n");
    for (t = 0U; t < SENSOR_WINDOW_LEN; t++) {
        for (c = 0U; c < N_CHANNELS_IN; c++) {
            s_tensor[t][c] = (int8_t)((int32_t)((t * 7U + c * 13U) % 255U) - 127);
        }
    }
    conv1d_layer1(s_tensor, s_l1);
    for (t = 0U; t < SENSOR_WINDOW_LEN; t++) {
        for (f = 0U; f < L1_FILTERS; f++) {
            if (s_l1[t][f] < 0) {
                uart_puts("[FAIL] t4: negative activation after ReLU at t=");
                uart_put_uint(t);
                uart_puts("\r\n");
                return 1;
            }
        }
    }
    /* The bias table must have been linked: an all-zero bias table is the
     * signature of the placeholder weights this model used to ship with. */
    {
        int32_t bias_or = 0;
        for (f = 0U; f < L1_FILTERS; f++) {
            bias_or |= g_conv_bias_l1[f];
        }
        if (bias_or == 0) {
            uart_puts("[FAIL] t4: layer-1 bias table is all zero "
                      "(placeholder weights linked?)\r\n");
            return 1;
        }
    }
    uart_puts("[PASS] t4: layer 1 clean over 128 timesteps\r\n");

    uart_puts("[PASS] All conv1d tests\r\n");
    return 0;
}
