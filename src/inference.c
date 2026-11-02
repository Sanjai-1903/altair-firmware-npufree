/*
 * inference.c — CPU Conv1D + SNN inference pipeline orchestration
 * Bare-metal RV32IM. No stdlib. No malloc. volatile on all register
 * accesses that must not be reordered or elided by the compiler.
 *
 * See inference.h for the pipeline description, and conv1d.h for the
 * compute kernel that replaced the NPU.
 */

#include <stdint.h>
#include "soc.h"
#include "snn.h"
#include "timer.h"
#include "latency.h"
#include "inference.h"
#include "sensor.h"
#include "features.h"
#include "conv1d.h"
#include "model_layout.h"

/*
 * Static scratch -- avoids growing the call stack per invocation; matches
 * this codebase's convention of static buffers for per-inference working
 * memory. Single-threaded, one inference in flight at a time.
 *
 * 3.8 KiB in .bss, out of DRAM's 64 KiB. The NPU build's s_image tile
 * staging buffer moved into conv1d.c with the code that fills it.
 */
static int16_t s_x[SENSOR_WINDOW_LEN];
static int16_t s_y[SENSOR_WINDOW_LEN];
static int16_t s_z[SENSOR_WINDOW_LEN];
static int8_t  s_tensor[SENSOR_WINDOW_LEN][N_CHANNELS_IN];
static int8_t  s_l1_out[SENSOR_WINDOW_LEN][L1_FILTERS];
static int8_t  s_pooled[POOL_LEN][L2_IN_CHANNELS];
static int8_t  s_l2_out[POOL_LEN][L2_FILTERS];
static int8_t  s_gap[L2_FILTERS];

/* Diagnostic breadcrumbs -- see inference.h. Two failure sites remain. */
volatile uint32_t g_inference_error_stage = 0U;
volatile uint32_t g_inference_error_index = 0xFFFFFFFFU;

void inference_init(void)
{
    /* Nothing accelerator-side to prepare: there is no weight SRAM to
     * preload now that the Conv1D tiles are read straight from .rodata
     * (see conv1d.c). Only the runtime sensor input path needs arming. */
    sensor_init();
}

/* popcount16() - count set bits in the low 16 bits of v. Small fixed-width
 * loop; Ibex RV32IM has no dedicated popcount instruction. */
static uint32_t popcount16(uint32_t v)
{
    uint32_t count = 0U;
    uint32_t i;

    for (i = 0U; i < 16U; i++) {
        count += (v >> i) & 1U;
    }
    return count;
}

int inference_run(void)
{
    uint32_t t0, t1, i;
    uint32_t spike_bits;

    /* Stage 1: real runtime input acquisition (UART, interrupt-driven --
     * see sensor.c). Blocks until a full window arrives or times out. */
    if (sensor_read_window(s_x, s_y, s_z) != 0) {
        g_inference_error_stage = INFERENCE_STAGE_SENSOR;
        g_inference_error_index = 0xFFFFFFFFU;
        return INFERENCE_ERROR;
    }

    /* Stage 2: raw samples -> quantized 128x4 tensor {x,y,z,magnitude}.
     * No statistics computed -- the CNN consumes the raw window. */
    tensor_build_and_quantize(s_x, s_y, s_z, s_tensor);

    /*
     * Latency window: feature quantization is outside it, the two Conv1D
     * layers and both pooling stages are inside it, and the SNN run is
     * outside it. That is deliberately the same window the NPU build
     * measured, so the reported numbers are directly comparable across
     * the two builds -- this window is exactly the work that moved from
     * the accelerator onto the CPU.
     */
    t0 = timer_get_cycles();

    /* Stages 3-6: Conv1D L1 -> MaxPool -> Conv1D L2 -> GlobalAvgPool.
     * All on the CPU; none of these can fail or time out. */
    conv1d_layer1(s_tensor, s_l1_out);
    maxpool1d_int8(s_l1_out, s_pooled);
    conv1d_layer2(s_pooled, s_l2_out);
    global_avg_pool_int8(s_l2_out, s_gap);

    t1 = timer_get_cycles();
    latency_update((t1 - t0) / (SYS_CLK_HZ / 1000000U));

    /* Stage 7: feed the SNN's input buffer -- feature values, not
     * pre-encoded spikes (see neuron_core.sv's ext_current_i / snn.h). */
    for (i = 0U; i < GAP_FEATURES; i++) {
        snn_write_input((uint8_t)i, s_gap[i]);
    }

    /* SNN configuration -- PROVISIONAL defaults (RTL reset values), not
     * calibrated against any trained model. connect_mask is left at its
     * reset default (all-ones) so the existing self-feedback path behaves
     * as it did before the input buffer existed; only the 16 input-mapped
     * neurons receive a nonzero ext_current_i. */
    snn_configure(0U /* weight_addr, ignored */, 20U /* threshold */,
                  1U /* leak */, 10U /* timesteps */,
                  0xFFFFFFFFU /* connect_mask[31:0] */);
    snn_launch();
    if (snn_poll_done() != 0) {
        g_inference_error_stage = INFERENCE_STAGE_SNN;
        g_inference_error_index = 0xFFFFFFFFU;
        return INFERENCE_ERROR;
    }

    /* Stage 8: decision. Read the 16 input-mapped neurons' final spike
     * state (SNN_SPIKE_OUT_0 bits [15:0] = spike_vector[15:0]) and apply
     * the provisional popcount rule -- see inference.h's
     * SNN_DECISION_THRESHOLD comment. */
    spike_bits = SNN_REG(SNN_SPIKE_OUT_0) & 0xFFFFU;
    return (popcount16(spike_bits) >= SNN_DECISION_THRESHOLD)
           ? INFERENCE_ANOMALY : INFERENCE_NORMAL;
}
