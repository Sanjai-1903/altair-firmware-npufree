#ifndef SENSOR_H
#define SENSOR_H

/*
 * sensor.h — Accelerometer window acquisition over UART
 * Edge-AI RISC-V SoC, bare-metal, no stdlib.
 *
 * This board (Arty A7-100T) has no accelerometer wired into soc_top.sv,
 * so "real input acquisition" here means a real runtime data path over
 * real hardware (UART RX, interrupt-driven, byte at a time) rather than a
 * hardcoded or permanently-zero buffer. A host (or, in simulation, the
 * cocotb testbench's UartDriver — see SoC/RTL_FIRMWARE/tests/common.py)
 * streams one window per inference as raw int16 samples; production
 * bring-up onto real accelerometer hardware only needs to replace this
 * file's byte source, not any of the fixed-point feature/Conv1D
 * pipeline downstream of it.
 *
 * Wire format (matches tools/generate_npu_test_vectors.py's sensor_XXX.bin):
 *   128 timesteps x (x, y, z), each int16 little-endian, milli-g units
 *   (1000 == 1.000 g). 768 bytes total per window.
 */

#include <stdint.h>

#define SENSOR_WINDOW_LEN   128U   /* samples per axis per window */

/** sensor_init() - Arm UART RX-done interrupts for sensor acquisition.
 * Call once after uart_init() and intc_init(), before the first
 * sensor_read_window() call. */
void sensor_init(void);

/**
 * sensor_read_window() - Blocking read of one 768-byte sensor window.
 * @x, @y, @z: SENSOR_WINDOW_LEN-element int16 output arrays (milli-g).
 * Returns 0 on success, -1 on UART RX timeout (~40 ms per byte with no
 * activity — see uart_irq_wait_rx()).
 */
int sensor_read_window(int16_t *x, int16_t *y, int16_t *z);

#endif /* SENSOR_H */
