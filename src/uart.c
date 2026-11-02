/*
 * uart.c - UART driver aligned to current RTL
 * No stdlib / POSIX headers. No printf.
 */

#include "uart.h"

/* ISR flags defined in trap.c; written only from M-mode trap context */
extern volatile uint32_t _uart_irq_rx_done;
extern volatile uint32_t _uart_irq_tx_done;
extern volatile uint32_t _uart_rx_byte;

void uart_init(uint32_t baud)
{
    /* CONTROL only carries IRQ-enable bits in the current RTL. */
    UART_REG(UART_CONTROL) = 0U;
    UART_REG(UART_BAUD) = BAUD_DIV(baud);
}

void uart_putc(char c)
{
    /* Wait until the transmitter is idle before launching the next byte. */
    while (UART_REG(UART_STATUS) & UART_STAT_TX_BUSY)
        ;
    UART_REG(UART_TX) = (uint32_t)(unsigned char)c;
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

char uart_getc(void)
{
    /* Current RTL exposes only the last received byte register. */
    return (char)(UART_REG(UART_RX) & 0xFFU);
}

int uart_rx_ready(void)
{
    /* RTL does not expose a pollable RX-ready bit. */
    return 0;
}

int uart_tx_ready(void)
{
    return (UART_REG(UART_STATUS) & UART_STAT_TX_BUSY) ? 0 : 1;
}

void uart_put_hex32(uint32_t val)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;

    uart_putc('0');
    uart_putc('x');
    for (i = 28; i >= 0; i -= 4)
        uart_putc(hex[(val >> i) & 0xFU]);
}

void uart_put_uint(uint32_t val)
{
    char buf[10];
    int idx = 0;

    if (val == 0U) {
        uart_putc('0');
        return;
    }

    while (val > 0U) {
        buf[idx++] = (char)('0' + (int)(val % 10U));
        val /= 10U;
    }

    while (--idx >= 0)
        uart_putc(buf[idx]);
}

void uart_irq_enable(uint32_t mask)
{
    UART_REG(UART_CONTROL) = mask;
}

void uart_irq_disable(void)
{
    UART_REG(UART_CONTROL) = 0U;
}

void uart_irq_flag_clear(void)
{
    _uart_irq_rx_done = 0U;
    _uart_irq_tx_done = 0U;
}

int uart_irq_wait_rx(void)
{
    uint32_t timeout = 0x100000UL;   /* ~2M cycles @ 50 MHz ~= 40 ms */

    while (!_uart_irq_rx_done) {
        if (--timeout == 0U)
            return -1;
    }
    return 0;
}

int uart_irq_wait_tx(void)
{
    uint32_t timeout = 0x100000UL;

    while (!_uart_irq_tx_done) {
        if (--timeout == 0U)
            return -1;
    }
    return 0;
}

uint8_t uart_irq_get_rx_byte(void)
{
    return (uint8_t)(_uart_rx_byte & 0xFFU);
}
