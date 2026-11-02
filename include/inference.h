#ifndef INFERENCE_H
#define INFERENCE_H

/*
 * inference.h — CPU Conv1D + SNN inference pipeline API
 * ALTair firmware, bare-metal, no stdlib.
 *
 * NPU-FREE BUILD. The deployed model is unchanged:
 *
 *   Conv1D(8, k=5, same, relu) -> MaxPool1D(2) ->
 *   Conv1D(16, k=3, same, relu) -> GlobalAveragePooling1D ->
 *   SNN input buffer -> SNN -> decision
 *
 * What changed is where the two Conv1D layers execute. They used to be
 * decomposed into 32 NPU launches, each one a DMA of a 256-byte im2col
 * tile into the NPU's activation SRAM followed by a launch and a
 * completion poll. They now run on the CPU in src/conv1d.c, from the same
 * tiles, producing bit-identical accumulators -- see conv1d.h for why
 * that identity is exact rather than approximate.
 *
 * Per-inference flow:
 *   sensor_read_window()        (UART, interrupt-driven -- sensor.c)
 *   tensor_build_and_quantize() (integer, {x,y,z,magnitude} -- features.c)
 *   conv1d_layer1()             16 tiles, bias + ReLU + requantize
 *   maxpool1d_int8()
 *   conv1d_layer2()             16 tiles, bias + ReLU + requantize
 *   global_avg_pool_int8()      -> 16 INT8 features
 *   snn_write_input() x16  ->  snn_launch()  ->  snn_poll_done()
 *   spike-vector readback -> decision -> telemetry (main.c)
 *
 * The SNN is now the only hardware accelerator in the pipeline, which is
 * also what the ALTair GF180 architecture document specifies.
 */

#include <stdint.h>
#include "soc.h"

/*
 * Diagnostic breadcrumbs -- set by inference.c right before any
 * INFERENCE_ERROR return, so main.c's error telemetry (and post-mortem
 * inspection) can report WHICH site failed.
 *
 *   g_inference_error_stage:
 *     1 = sensor window acquisition timed out
 *     6 = SNN never asserted DONE
 *   g_inference_error_index: reserved; 0xFFFFFFFF (neither remaining
 *     failure site is one of a numbered series).
 *
 * Stage codes 2..5 are RETIRED, not renumbered: they were L1 DMA, L1 NPU,
 * L2 DMA and L2 NPU. Those stages have no hardware left to wait on --
 * conv1d_tile() is a pure function that cannot time out -- so they can no
 * longer be reported. The surviving codes keep their old values so an
 * existing telemetry decoder stays correct; a decoder that sees 2..5 is
 * reading output from an older, NPU-equipped build.
 */
extern volatile uint32_t g_inference_error_stage;
extern volatile uint32_t g_inference_error_index;

#define INFERENCE_STAGE_SENSOR   1U
#define INFERENCE_STAGE_SNN      6U

/* ── Classification results ─────────────────────────────────────────────── */
#define INFERENCE_NORMAL    0
#define INFERENCE_ANOMALY   1
#define INFERENCE_ERROR    (-1)

/*
 * SNN decision rule -- PROVISIONAL, not an architectural commitment, and
 * NOT calibrated. SNN_DECISION_THRESHOLD (K) is a placeholder (a majority
 * of SNN_NUM_INPUT_NEURONS); it is not derived from any trained model, and
 * the SNN threshold/leak/timestep values this firmware programs are the
 * RTL reset values. No detection accuracy can be attached to this build's
 * decision output -- that calibration is open work (project blocker BK7).
 * The removal of the NPU does not change this either way: the rule read
 * the SNN's spike vector before and still does.
 */
#define SNN_DECISION_THRESHOLD   8U

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * inference_init() - Arm the sensor UART RX path. Must be called once
 * after snn_init(), before the first inference_run(). There is no weight
 * preload step any more: the Conv1D weight tiles are read directly from
 * .rodata by src/conv1d.c, so nothing has to be copied into accelerator
 * SRAM at boot or per layer.
 */
void inference_init(void);

/**
 * inference_run() - Block for one sensor window, then run the full
 * Conv1D + SNN forward pass.
 * Returns INFERENCE_NORMAL, INFERENCE_ANOMALY, or INFERENCE_ERROR
 * (sensor or SNN timeout; see g_inference_error_stage).
 */
int inference_run(void);

#endif /* INFERENCE_H */
