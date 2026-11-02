/*
 * test_uart.c - UART TX smoke test
 * Current RTL guarantees TX launch and TX-busy status only.
 * It does not expose a firmware-visible RX-ready bit for safe polling.
 */

#include "uart.h"

static const char test_str[] = "UART_TX_OK";

/* Returns 0 on pass, non-zero on failure. */
int test_uart(void)
{
    const char *p = test_str;

    uart_init(115200U);

    while (*p) {
        uart_putc(*p);
        p++;
    }

    uart_puts("\r\n[PASS] UART TX smoke\r\n");
    uart_puts("[INFO] RX-ready polling is RTL NOT IMPLEMENTED\r\n");
    return 0;
}

int main(void)
{
    return test_uart();
}
