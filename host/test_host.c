/*
 * test_host.c — native verification harness for the NPU-free firmware.
 *
 * Builds the real firmware sources (features.c, conv1d.c, inference.c,
 * snn.c, uart.c, timer.c, intc.c, telemetry.c, crc16.c, latency.c)
 * against the host device model in mock_soc.c, and checks them against
 * references written independently in this file.
 *
 * WHAT THIS PROVES
 *   - conv1d_tile() equals a plain, unoptimised 8x32-by-32x8 INT8 matmul
 *     for random tiles, i.e. the zero-row/zero-weight skips in conv1d.c
 *     are exact and not an approximation. This is the core claim of the
 *     NPU removal: the CPU kernel reproduces the accelerator's arithmetic.
 *   - conv1d_layer1()/conv1d_layer2(), driven through the real im2col
 *     tiling, equal a direct textbook 'same'-padded Conv1D computed
 *     straight from the weight tiles, including bias, ReLU and
 *     requantization. The tiling decomposition is therefore right.
 *   - maxpool1d_int8() and global_avg_pool_int8() match independent
 *     references.
 *   - inference_run() completes end to end, is deterministic for a given
 *     input window, and reports the sensor-timeout error path correctly.
 *   - telemetry frames decode, with the CRC recomputed by an independent
 *     table-driven implementation rather than by the firmware's own.
 *   - nothing in the firmware touches 0x2000_0000..0x2000_FFFF, the
 *     window the removed NPU occupied: the device model aborts on any
 *     access to an unmapped address, so a surviving NPU access would
 *     fail these tests rather than pass quietly.
 *
 * WHAT THIS DOES NOT PROVE
 *   Anything about the RTL, about timing, about the SNN's real behaviour
 *   (the device model's LIF is a stand-in -- see mock_soc.c), or about
 *   detection accuracy (the decision threshold is uncalibrated; project
 *   blocker BK7). It is a software regression test, not hardware evidence,
 *   and it is not the bit-exactness gate G4 -- that gate needs Person 2's
 *   golden vectors, which do not exist in the repository yet (BK6).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "soc.h"
#include "uart.h"
#include "intc.h"
#include "timer.h"
#include "dma.h"
#include "snn.h"
#include "gpio.h"
#include "spi.h"
#include "i2c.h"
#include "pwm.h"
#include "sensor.h"
#include "features.h"
#include "conv1d.h"
#include "inference.h"
#include "latency.h"
#include "telemetry.h"
#include "mock_soc.h"

extern const unsigned char g_conv_weight_l1[TILE_BYTES];
extern const unsigned char g_conv_weight_l2_g0[TILE_BYTES];
extern const unsigned char g_conv_weight_l2_g1[TILE_BYTES];
extern const int32_t g_conv_bias_l1[L1_FILTERS];
extern const int32_t g_conv_bias_l2[L2_FILTERS];

static int g_failures;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

/* ── Deterministic PRNG (xorshift32) so failures are reproducible ─────── */
static uint32_t rng_state = 0x12345678U;
static uint32_t rnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}
static int8_t rnd_i8(void)
{
    /* symmetric int8 range, matching the quantization contract */
    return (int8_t)((int32_t)(rnd() % 255U) - 127);
}

/* ── Independent references ───────────────────────────────────────────── */

/* Plain signed right shift with round-to-nearest, sign-magnitude. Written
 * from the contract in model_layout.h, not copied from features.c. */
static int32_t ref_round_shift(int32_t v, int shift)
{
    if (shift <= 0) return v;
    {
        int32_t half = (int32_t)1 << (shift - 1);
        if (v >= 0) return (v + half) >> shift;
        return -(((-v) + half) >> shift);
    }
}

static int8_t ref_clamp_i8(int32_t v)
{
    if (v >  127) return  127;
    if (v < -127) return -127;
    return (int8_t)v;
}

/* The accelerator's primitive, written out literally with no skips. */
static void ref_matmul(const uint8_t *w, const uint8_t *a, int32_t *acc)
{
    uint32_t f, j, r;

    for (f = 0; f < TILE_LANES; f++) {
        for (j = 0; j < TILE_LANES; j++) {
            int32_t sum = 0;
            for (r = 0; r < TILE_ROWS; r++) {
                sum += (int32_t)(int8_t)w[r * TILE_LANES + f] *
                       (int32_t)(int8_t)a[r * TILE_LANES + j];
            }
            acc[f * TILE_LANES + j] = sum;
        }
    }
}

/* Textbook 'same'-padded Conv1D straight from a weight tile, bypassing
 * im2col entirely: out[t][f] = requant(relu(bias[f] + sum_k sum_c
 * in[t + k - K/2][c] * W[(k * C + c) * 8 + f])). */
static void ref_conv1d(const int8_t *in, uint32_t t_len, uint32_t ch_in,
                       const uint8_t *w, const int32_t *bias,
                       uint32_t kernel, uint32_t filters,
                       uint32_t filter_lane_base, int shift,
                       int8_t *out, uint32_t out_stride)
{
    uint32_t t, f, k, c;

    for (t = 0; t < t_len; t++) {
        for (f = 0; f < filters; f++) {
            int32_t sum = 0;
            for (k = 0; k < kernel; k++) {
                int32_t src = (int32_t)t + (int32_t)k - (int32_t)(kernel / 2U);
                if (src < 0 || src >= (int32_t)t_len) continue;
                for (c = 0; c < ch_in; c++) {
                    int32_t x = (int32_t)in[(uint32_t)src * ch_in + c];
                    int32_t wv = (int32_t)(int8_t)
                        w[(k * ch_in + c) * TILE_LANES + f];
                    sum += x * wv;
                }
            }
            sum += bias[filter_lane_base + f];
            if (sum < 0) sum = 0;
            out[t * out_stride + filter_lane_base + f] =
                ref_clamp_i8(ref_round_shift(sum, shift));
        }
    }
}

/* CRC16-Modbus, nibble-table driven -- a different implementation from
 * telemetry/crc16.c's bit-by-bit loop. */
static uint16_t ref_crc16(const uint8_t *d, size_t n)
{
    static const uint16_t tbl[16] = {
        0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
        0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400
    };
    uint16_t crc = 0xFFFFU;
    size_t i;

    for (i = 0; i < n; i++) {
        crc = (uint16_t)((crc >> 4) ^ tbl[(crc ^ d[i]) & 0x0FU]);
        crc = (uint16_t)((crc >> 4) ^ tbl[(crc ^ (d[i] >> 4)) & 0x0FU]);
    }
    return crc;
}

/* ── Test 1: the kernel equals the accelerator primitive ──────────────── */
static void t1_tile_matches_plain_matmul(void)
{
    uint8_t w[TILE_BYTES], a[TILE_BYTES];
    int32_t got[CONV1D_TILE_ACCS], want[CONV1D_TILE_ACCS];
    uint32_t trial, i, mismatches = 0;

    printf("T1 conv1d_tile() == unoptimised 8x32*32x8 INT8 matmul\n");

    for (trial = 0; trial < 500U; trial++) {
        /* A quarter of the trials use the real weight tiles so the
         * structural zero padding (rows 20..31 / 24..31) is covered, and
         * the rest are dense random so the skips get no free pass. */
        if (trial % 4U == 0U) {
            memcpy(w, g_conv_weight_l1, TILE_BYTES);
        } else if (trial % 4U == 1U) {
            memcpy(w, g_conv_weight_l2_g1, TILE_BYTES);
        } else {
            for (i = 0; i < TILE_BYTES; i++) w[i] = (uint8_t)rnd_i8();
        }
        for (i = 0; i < TILE_BYTES; i++) a[i] = (uint8_t)rnd_i8();

        conv1d_tile(w, a, got);
        ref_matmul(w, a, want);
        for (i = 0; i < CONV1D_TILE_ACCS; i++) {
            if (got[i] != want[i]) mismatches++;
        }
    }
    CHECK(mismatches == 0, "%u accumulator mismatches over 500 tiles",
          mismatches);
    if (mismatches == 0) {
        printf("  ok: 500 tiles x 64 accumulators, zero mismatches\n");
    }

    /* Saturation headroom: the worst case must stay inside INT32. */
    for (i = 0; i < TILE_BYTES; i++) { w[i] = 127; a[i] = 127; }
    conv1d_tile(w, a, got);
    CHECK(got[0] == 32 * 127 * 127, "worst-case accumulator %d, expected %d",
          got[0], 32 * 127 * 127);
    printf("  ok: worst-case accumulator %d (INT32 max 2147483647)\n", got[0]);
}

/* ── Test 2: layer 1 through the real tiling == textbook conv ─────────── */
static void t2_layer1_matches_reference(void)
{
    static int8_t tensor[SENSOR_WINDOW_LEN][N_CHANNELS_IN];
    static int8_t got[SENSOR_WINDOW_LEN][L1_FILTERS];
    static int8_t want[SENSOR_WINDOW_LEN][L1_FILTERS];
    uint32_t t, c, f, mismatches = 0;

    printf("T2 conv1d_layer1() == textbook same-padded Conv1D(8,k=5)\n");

    for (t = 0; t < SENSOR_WINDOW_LEN; t++)
        for (c = 0; c < N_CHANNELS_IN; c++)
            tensor[t][c] = rnd_i8();

    conv1d_layer1(tensor, got);
    memset(want, 0, sizeof want);
    ref_conv1d(&tensor[0][0], SENSOR_WINDOW_LEN, N_CHANNELS_IN,
               g_conv_weight_l1, g_conv_bias_l1, L1_KERNEL, L1_FILTERS,
               0, L1_REQUANT_SHIFT, &want[0][0], L1_FILTERS);

    for (t = 0; t < SENSOR_WINDOW_LEN; t++)
        for (f = 0; f < L1_FILTERS; f++)
            if (got[t][f] != want[t][f]) mismatches++;

    CHECK(mismatches == 0, "%u of %u layer-1 activations differ",
          mismatches, SENSOR_WINDOW_LEN * L1_FILTERS);
    if (mismatches == 0)
        printf("  ok: %u activations identical\n",
               SENSOR_WINDOW_LEN * L1_FILTERS);
}

/* ── Test 3: layer 2, both filter groups ──────────────────────────────── */
static void t3_layer2_matches_reference(void)
{
    static int8_t pooled[POOL_LEN][L2_IN_CHANNELS];
    static int8_t got[POOL_LEN][L2_FILTERS];
    static int8_t want[POOL_LEN][L2_FILTERS];
    uint32_t t, c, f, mismatches = 0;

    printf("T3 conv1d_layer2() == textbook same-padded Conv1D(16,k=3)\n");

    for (t = 0; t < POOL_LEN; t++)
        for (c = 0; c < L2_IN_CHANNELS; c++)
            pooled[t][c] = rnd_i8();

    conv1d_layer2(pooled, got);
    memset(want, 0, sizeof want);
    /* Filter group 0 -> output filters 0..7, group 1 -> 8..15. */
    ref_conv1d(&pooled[0][0], POOL_LEN, L2_IN_CHANNELS,
               g_conv_weight_l2_g0, g_conv_bias_l2, L2_KERNEL, TILE_LANES,
               0, L2_REQUANT_SHIFT, &want[0][0], L2_FILTERS);
    ref_conv1d(&pooled[0][0], POOL_LEN, L2_IN_CHANNELS,
               g_conv_weight_l2_g1, g_conv_bias_l2, L2_KERNEL, TILE_LANES,
               TILE_LANES, L2_REQUANT_SHIFT, &want[0][0], L2_FILTERS);

    for (t = 0; t < POOL_LEN; t++)
        for (f = 0; f < L2_FILTERS; f++)
            if (got[t][f] != want[t][f]) mismatches++;

    CHECK(mismatches == 0, "%u of %u layer-2 activations differ",
          mismatches, POOL_LEN * L2_FILTERS);
    if (mismatches == 0)
        printf("  ok: %u activations identical\n", POOL_LEN * L2_FILTERS);
}

/* ── Test 4: pooling stages ───────────────────────────────────────────── */
static void t4_pooling_matches_reference(void)
{
    static int8_t l1[SENSOR_WINDOW_LEN][L1_FILTERS];
    static int8_t pooled[POOL_LEN][L2_IN_CHANNELS];
    static int8_t l2[POOL_LEN][L2_FILTERS];
    int8_t gap[L2_FILTERS];
    uint32_t t, c, bad = 0;

    printf("T4 maxpool1d_int8() and global_avg_pool_int8()\n");

    for (t = 0; t < SENSOR_WINDOW_LEN; t++)
        for (c = 0; c < L1_FILTERS; c++)
            l1[t][c] = rnd_i8();

    maxpool1d_int8(l1, pooled);
    for (t = 0; t < POOL_LEN; t++) {
        for (c = 0; c < L2_IN_CHANNELS; c++) {
            int8_t a = l1[t * 2U][c], b = l1[t * 2U + 1U][c];
            int8_t want = (a > b) ? a : b;
            if (pooled[t][c] != want) bad++;
        }
    }
    CHECK(bad == 0, "%u maxpool outputs differ", bad);

    for (t = 0; t < POOL_LEN; t++)
        for (c = 0; c < L2_FILTERS; c++)
            l2[t][c] = rnd_i8();

    global_avg_pool_int8(l2, gap);
    bad = 0;
    for (c = 0; c < L2_FILTERS; c++) {
        int32_t total = 0;
        for (t = 0; t < POOL_LEN; t++) total += (int32_t)l2[t][c];
        if (gap[c] != ref_clamp_i8(ref_round_shift(total, 6))) bad++;
    }
    CHECK(bad == 0, "%u GAP outputs differ", bad);
    if (bad == 0) printf("  ok: both pooling stages match\n");
}

/* ── Test 5: full pipeline, determinism, and the error path ───────────── */
static void make_window(int16_t *x, int16_t *y, int16_t *z, int32_t amp)
{
    uint32_t t;
    for (t = 0; t < SENSOR_WINDOW_LEN; t++) {
        /* milli-g, integer, no float anywhere */
        x[t] = (int16_t)(amp * (int32_t)(t % 17U) - 400);
        y[t] = (int16_t)(amp * (int32_t)(t % 11U) - 250);
        z[t] = (int16_t)(1000 + amp * (int32_t)(t % 7U));
    }
}

static void bring_up(void)
{
    mock_soc_reset();
    uart_init(115200U);
    intc_init();
    timer_init(0U);
    dma_init();
    gpio_init();
    gpio_set_output(GPIO_PIN_HEARTBEAT);
    spi_init(4U);
    i2c_init(9U);
    pwm_init();
    snn_init();
    inference_init();
    latency_reset();
}

static void t5_inference_end_to_end(void)
{
    int16_t x[SENSOR_WINDOW_LEN], y[SENSOR_WINDOW_LEN], z[SENSOR_WINDOW_LEN];
    int r1, r2, r3;
    uint32_t last, avg, mn, mx;

    printf("T5 inference_run() end to end\n");

    bring_up();
    make_window(x, y, z, 7);

    mock_sensor_push_window(x, y, z);
    r1 = inference_run();
    CHECK(r1 != INFERENCE_ERROR, "first inference returned ERROR, stage %u",
          g_inference_error_stage);
    CHECK(r1 == INFERENCE_NORMAL || r1 == INFERENCE_ANOMALY,
          "unexpected result %d", r1);

    mock_sensor_push_window(x, y, z);
    r2 = inference_run();
    CHECK(r1 == r2, "same window gave different decisions: %d then %d",
          r1, r2);
    CHECK(mock_snn_launch_count() == 2U,
          "expected 2 SNN launches, device model saw %u",
          mock_snn_launch_count());
    printf("  ok: decision %s, reproducible, 2 SNN launches\n",
           r1 == INFERENCE_ANOMALY ? "ANOMALY" : "NORMAL");

    CHECK(latency_get_stats(&last, &avg, &mn, &mx) == 1,
          "latency stats report no samples after two inferences");
    CHECK(latency_get_count() == 2U, "latency count %u, expected 2",
          latency_get_count());
    printf("  ok: latency samples recorded (count %u)\n",
           latency_get_count());

    /* Error path: no window queued -> sensor stage must be reported. */
    mock_sensor_starve();
    g_inference_error_stage = 0U;
    r3 = inference_run();
    CHECK(r3 == INFERENCE_ERROR, "starved sensor returned %d, expected ERROR",
          r3);
    CHECK(g_inference_error_stage == INFERENCE_STAGE_SENSOR,
          "error stage %u, expected %u", g_inference_error_stage,
          (unsigned)INFERENCE_STAGE_SENSOR);
    printf("  ok: sensor timeout reported as stage %u\n",
           g_inference_error_stage);

    /* A different window should still complete; it may or may not change
     * the decision, which is not something this fixture can assert. */
    make_window(x, y, z, 31);
    mock_sensor_push_window(x, y, z);
    CHECK(inference_run() != INFERENCE_ERROR,
          "second window returned ERROR, stage %u", g_inference_error_stage);
    printf("  ok: pipeline recovers after an error\n");
}

/* ── Test 6: telemetry framing decodes, CRC independently verified ────── */
static void t6_telemetry_frames(void)
{
    const uint8_t *tx;
    size_t n, i = 0;
    uint32_t frames = 0, bad_crc = 0, bad_delim = 0;
    int saw_inference = 0, saw_latency = 0, saw_error = 0;

    printf("T6 telemetry framing and CRC\n");

    bring_up();
    mock_uart_tx_reset();
    telemetry_init();
    telemetry_send_inference((uint8_t)INFERENCE_ANOMALY);
    telemetry_send_latency(1234U, 2345U, 1000U, 9999U);
    telemetry_send_error((uint8_t)INFERENCE_STAGE_SNN);
    telemetry_send_heartbeat(42U);

    tx = mock_uart_tx_data();
    n  = mock_uart_tx_len();

    while (i < n) {
        uint8_t len, type;
        uint16_t crc_rx, crc_calc;
        uint8_t scratch[256 + 1];

        if (tx[i] != 0x7EU) { bad_delim++; i++; continue; }
        if (i + 5 > n) break;
        len  = tx[i + 1];
        type = tx[i + 2];
        if (i + 3 + len + 3 > n) { bad_delim++; break; }

        scratch[0] = type;
        memcpy(scratch + 1, tx + i + 3, len);
        crc_calc = ref_crc16(scratch, (size_t)len + 1U);
        crc_rx = (uint16_t)(tx[i + 3 + len] |
                            ((uint16_t)tx[i + 4 + len] << 8));
        if (crc_rx != crc_calc) bad_crc++;
        if (tx[i + 5 + len] != 0x7FU) bad_delim++;

        if (type == PKT_INFERENCE) {
            saw_inference = 1;
            CHECK(len == 1U && tx[i + 3] == (uint8_t)INFERENCE_ANOMALY,
                  "inference payload wrong");
        } else if (type == PKT_LATENCY) {
            saw_latency = 1;
            CHECK(len == 16U, "latency payload %u bytes, expected 16", len);
            CHECK(tx[i + 3] == 0xD2U && tx[i + 4] == 0x04U,
                  "latency last_us not 1234 little-endian");
        } else if (type == PKT_ERROR) {
            saw_error = 1;
            CHECK(len == 1U && tx[i + 3] == (uint8_t)INFERENCE_STAGE_SNN,
                  "error payload wrong");
        }

        frames++;
        i += (size_t)6U + len;
    }

    CHECK(frames == 4U, "decoded %u frames, expected 4", frames);
    CHECK(bad_crc == 0U, "%u frames failed the independent CRC check",
          bad_crc);
    CHECK(bad_delim == 0U, "%u framing/delimiter problems", bad_delim);
    CHECK(saw_inference && saw_latency && saw_error,
          "missing a packet type (inference %d latency %d error %d)",
          saw_inference, saw_latency, saw_error);
    if (!bad_crc && !bad_delim && frames == 4U)
        printf("  ok: 4 frames, CRC16-Modbus verified independently\n");
}

int main(void)
{
    printf("ALTair firmware host harness (NPU-free build)\n");
    printf("fixture only -- not RTL evidence, not gate G4\n\n");

    t1_tile_matches_plain_matmul();
    t2_layer1_matches_reference();
    t3_layer2_matches_reference();
    t4_pooling_matches_reference();
    t5_inference_end_to_end();
    t6_telemetry_frames();

    printf("\n");
    if (g_failures == 0) {
        printf("PASS: all host checks passed\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
