/*
 * model_layout.h — frozen shape + fixed-point contract of the deployed
 * Conv1D front-end.
 *
 * Originally generated as npu_weights_layout.h by
 * tools/extract_npu_weights.py alongside src/model_weights_data.c (then
 * npu_weights_data.c). Regenerate both together with:
 *   python train.py && python tools/extract_npu_weights.py
 *
 * Deployed model (unchanged by the NPU removal):
 *   Conv1D(8, k=5, same, relu) -> MaxPool1D(2) ->
 *   Conv1D(16, k=3, same, relu) -> GlobalAveragePooling1D
 * GAP's 16-value output feeds the SNN's input buffer (include/snn.h).
 * There is no Dense head in the deployed pipeline.
 *
 * NPU-FREE BUILD. The numbers below are unchanged from the NPU build and
 * must stay bit-identical to npu_model.py -- they are the model's
 * quantization contract, not a property of whatever executes it. What
 * changed is only WHO executes the matmul: src/conv1d.c on the CPU
 * instead of 32 NPU launches. The TILE_* geometry is retained because the
 * im2col tiling (32 contraction rows x 8 lanes) is what the weight images
 * in src/model_weights_data.c are laid out for, so keeping it means the
 * existing weight images and golden vectors still apply unchanged.
 *
 * Macro renames from npu_weights_layout.h (see docs/NPU_REMOVAL.md):
 *   NPU_L1_FILTERS -> L1_FILTERS         NPU_IMAGE_BYTES -> TILE_BYTES
 *   NPU_L2_FILTERS -> L2_FILTERS         NPU_INPUT_SHIFT -> INPUT_QUANT_SHIFT
 *   NPU_GAP_FEATURES -> GAP_FEATURES     NPU_Lx_REQUANT_SHIFT -> Lx_REQUANT_SHIFT
 */

#ifndef MODEL_LAYOUT_H_
#define MODEL_LAYOUT_H_

#include <stdint.h>

/* ── Layer shapes ───────────────────────────────────────────────────────── */
#define L1_FILTERS               8U
#define L1_KERNEL                5U
#define L2_FILTERS               16U
#define L2_KERNEL                3U
#define GAP_FEATURES             16U

/* ── im2col tile geometry ───────────────────────────────────────────────────
 * One tile = TILE_ROWS contraction rows x TILE_LANES lanes, one byte per
 * entry. A weight tile's lane index is the filter; an activation tile's
 * lane index is the output timestep. Both are addressed as
 * tile[row * TILE_LANES + lane] -- see conv1d.h for the exact dot product.
 * ─────────────────────────────────────────────────────────────────────────── */
#define TILE_ROWS                32U
#define TILE_LANES               8U
#define TILE_BYTES               256U   /* TILE_ROWS * TILE_LANES */

/* ── Fixed-point contract ───────────────────────────────────────────────────
 * Stage-1 quantization: q = clamp(round_shift(milli_g_value,
 * INPUT_QUANT_SHIFT), -127, 127). Must match
 * npu_model.build_input_tensor() exactly -- see src/features.c.
 * ─────────────────────────────────────────────────────────────────────────── */
#define INPUT_QUANT_SHIFT        5

/* Post-accumulate, post-bias, post-ReLU requantization shifts. Must match
 * npu_model.conv1d_layer{1,2}_forward() exactly. In the NPU build the
 * accelerator applied ReLU in hardware and firmware did the bias-add and
 * requantization; now src/conv1d.c does all three, in that same order
 * (accumulate -> bias -> ReLU -> requantize), which is what the reference
 * does. */
#define L1_REQUANT_SHIFT         8
#define L2_REQUANT_SHIFT         8

#endif  /* MODEL_LAYOUT_H_ */
