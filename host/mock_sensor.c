/*
 * mock_sensor.c — host stand-in for src/sensor.c. TEST FIXTURE ONLY.
 *
 * sensor.h already documents the byte source as the one layer to replace
 * for real accelerometer bring-up ("production bring-up onto real
 * accelerometer hardware only needs to replace this file's byte source,
 * not any of the fixed-point feature pipeline downstream of it"). This is
 * that replacement, for the host: it serves windows handed to it by the
 * test runner instead of reading UART RX bytes under interrupt.
 *
 * Everything downstream of here -- quantization, Conv1D, pooling, the SNN
 * hand-off, the decision and the telemetry framing -- is the real
 * firmware source, unmodified.
 */

#include <string.h>

#include "sensor.h"
#include "mock_soc.h"

static int16_t s_qx[SENSOR_WINDOW_LEN];
static int16_t s_qy[SENSOR_WINDOW_LEN];
static int16_t s_qz[SENSOR_WINDOW_LEN];
static int     s_have_window;

void sensor_init(void)
{
    /* Nothing to arm: no UART RX interrupt path in a host build. */
}

int sensor_read_window(int16_t *x, int16_t *y, int16_t *z)
{
    if (!s_have_window) {
        return -1;      /* same failure the real driver reports on timeout */
    }
    memcpy(x, s_qx, sizeof s_qx);
    memcpy(y, s_qy, sizeof s_qy);
    memcpy(z, s_qz, sizeof s_qz);
    s_have_window = 0;  /* one window per queued window, like the real path */
    return 0;
}

void mock_sensor_push_window(const int16_t *x, const int16_t *y,
                             const int16_t *z)
{
    memcpy(s_qx, x, sizeof s_qx);
    memcpy(s_qy, y, sizeof s_qy);
    memcpy(s_qz, z, sizeof s_qz);
    s_have_window = 1;
}

void mock_sensor_starve(void)
{
    s_have_window = 0;
}
