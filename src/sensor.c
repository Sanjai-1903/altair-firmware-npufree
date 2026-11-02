/*
 * sensor.c — Accelerometer window acquisition over UART
 * No stdlib / POSIX headers.
 */

#include "sensor.h"
#include "uart.h"
#include "intc.h"

void sensor_init(void)
{
    uart_irq_enable(UART_CTRL_IRQ_RX_DONE);   /* arm RX-done at the peripheral */
    intc_enable(IRQ_UART);                    /* unmask UART at the controller */
}

static int sensor_read_byte(uint8_t *out)
{
    uart_irq_flag_clear();
    if (uart_irq_wait_rx() != 0) {
        return -1;                            /* timeout — no byte arrived */
    }
    *out = uart_irq_get_rx_byte();
    return 0;
}

static int sensor_read_i16(int16_t *out)
{
    uint8_t lo, hi;

    if (sensor_read_byte(&lo) != 0) {
        return -1;
    }
    if (sensor_read_byte(&hi) != 0) {
        return -1;
    }
    *out = (int16_t)(uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));  /* little-endian */
    return 0;
}

int sensor_read_window(int16_t *x, int16_t *y, int16_t *z)
{
    uint32_t t;

    for (t = 0U; t < SENSOR_WINDOW_LEN; t++) {
        if (sensor_read_i16(&x[t]) != 0) {
            return -1;
        }
        if (sensor_read_i16(&y[t]) != 0) {
            return -1;
        }
        if (sensor_read_i16(&z[t]) != 0) {
            return -1;
        }
    }
    return 0;
}
