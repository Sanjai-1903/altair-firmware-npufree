#ifndef MOCK_SOC_H
#define MOCK_SOC_H

/*
 * mock_soc.h — host-side device model for the ALTair firmware.
 *
 * THIS IS A TEST FIXTURE, NOT A MODEL OF THE RTL. It exists so the
 * firmware's own integer pipeline (quantization, Conv1D, pooling,
 * telemetry framing, latency accounting, error paths) can be compiled and
 * executed with a host compiler, with no RISC-V toolchain and no
 * simulator. Peripheral behaviour here is the minimum that keeps the real
 * drivers running; where the real RTL's timing or semantics are subtle,
 * this model is simply wrong, on purpose. Nothing it reports is evidence
 * about hardware. See host/README.md.
 */

#include <stdint.h>
#include <stddef.h>

/* ── UART TX capture ─────────────────────────────────────────────────────── */
void            mock_uart_tx_reset(void);
size_t          mock_uart_tx_len(void);
const uint8_t  *mock_uart_tx_data(void);

/* ── Sensor window feed (consumed by host/mock_sensor.c) ─────────────────── */
/* Queue one window of SENSOR_WINDOW_LEN x {x,y,z} int16 samples. */
void mock_sensor_push_window(const int16_t *x, const int16_t *y,
                             const int16_t *z);
/* Drop any queued window so the next sensor_read_window() fails, which is
 * how the sensor-timeout error path is exercised. */
void mock_sensor_starve(void);

/* ── SNN device model ────────────────────────────────────────────────────── */
/* Final spike vector of the most recent run, bits [31:0]. */
uint32_t mock_snn_last_spikes(void);
/* How many times START has been accepted since reset. */
uint32_t mock_snn_launch_count(void);

/* ── Whole-model reset between test cases ───────────────────────────────── */
void mock_soc_reset(void);

/* Force the pending-write check (see mock_soc.c's access-hook comment).
 * The UART capture accessors above already do this for you. */
void mock_soc_sync(void);

#endif /* MOCK_SOC_H */
