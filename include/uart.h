#ifndef UART_H
#define UART_H

/*
 * uart.h - UART driver API aligned to current RTL
 * No stdlib / POSIX headers. No printf.
 *
 * Verified RTL contract:
 *   UART_TX      0x00 write launches a transfer
 *   UART_RX      0x04 returns the most recently captured byte
 *   UART_STATUS  0x08 bit 0 = tx_busy
 *   UART_CONTROL 0x0C bit 0 = TX-done IRQ enable
 *   UART_CONTROL 0x0C bit 1 = RX-done IRQ enable
 *   UART_BAUD    0x10 baud divider
 *
 * Current RTL does not expose firmware-visible enable bits, RX-ready,
 * FIFO state, parity state, or error status.
 */

#include <stdint.h>
#include "soc.h"

#define UART_TX               0x00U
#define UART_RX               0x04U
#define UART_STATUS           0x08U
#define UART_CONTROL          0x0CU
#define UART_BAUD             0x10U

#define UART_STAT_TX_BUSY     (1U << 0)

#define UART_CTRL_IRQ_TX_DONE (1U << 0)
#define UART_CTRL_IRQ_RX_DONE (1U << 1)

#define BAUD_DIV(baud)        (SYS_CLK_HZ / (baud) - 1U)
#define UART_REG(offset)      REG(UART_BASE, offset)

/* Program the baud divider and leave UART IRQs disabled. */
void uart_init(uint32_t baud);

/* Blocking transmit: wait for !TX_BUSY before writing the next byte. */
void uart_putc(char c);

/* Transmit a null-terminated string via uart_putc(). */
void uart_puts(const char *s);

/* Return the most recently captured RX byte.
 * Current RTL does not expose a safe RX-ready polling indicator. */
char uart_getc(void);

/* Returns 0 because current RTL does not expose a pollable RX-ready bit. */
int uart_rx_ready(void);

/* Returns 1 when TX is idle, else 0. */
int uart_tx_ready(void);

/* Print "0x" plus 8 uppercase hex digits. */
void uart_put_hex32(uint32_t val);

/* Print decimal unsigned integer. */
void uart_put_uint(uint32_t val);

/*
 * INTC-driven interrupt path (trap.c:uart_irq_service).
 *
 * uart_irq_o combines TX-done and RX-done pulses into a single line with
 * no per-source status bit (RTL_GAPS #6) — the ISR uses the enable bit(s)
 * armed via uart_irq_enable() to decide what to service. Call
 * intc_enable(IRQ_UART) + intc_global_enable() separately.
 */

/* Program UART_CONTROL directly (UART_CTRL_IRQ_TX_DONE / _RX_DONE, OR'd). */
void uart_irq_enable(uint32_t mask);
void uart_irq_disable(void);
void uart_irq_flag_clear(void);
int uart_irq_wait_rx(void);
int uart_irq_wait_tx(void);
/* Last byte captured by the RX-done ISR (no FIFO — see UART_RX). */
uint8_t uart_irq_get_rx_byte(void);

#endif /* UART_H */
