/*
 * test_spi.c — SPI master driver / RTL test
 * Links as a standalone ELF with its own main().
 * No stdlib, no printf.
 *
 * Co-designed with SoC/RTL_FIRMWARE/tests/test_spi.py: the cocotb bench
 * acts as the SPI slave, drives 0xCAFEF00D on MISO (MSB first, one bit
 * per SCLK falling edge) and samples MOSI on rising edges expecting
 * 0xDEADBEEF. Run standalone (spi_miso_i tied low) RX reads 0x00000000
 * and this image prints FAIL — that is expected outside the bench.
 *
 * Exercises the verified RTL contract (spi_top.sv, spi_master.sv,
 * spi_regs.sv): CLKDIV write, CONTROL (IRQ-enable) write, TX write
 * triggering a fixed 32-bit transfer, BUSY polling, RX readback,
 * hardware CS assertion, done-IRQ pulse.
 */

#include "uart.h"
#include "spi.h"

int main(void)
{
    uint32_t rx;

    uart_init(115200U);
    uart_puts("spi test start\r\n");

    spi_init(4U);                      /* CLKDIV=4: SCLK = 50 MHz / 10 */
    SPI_REG(SPI_REG_CONTROL) = 1U;     /* done-IRQ pulse, observed by bench */

    rx = spi_transfer(0xDEADBEEFUL);

    uart_puts("spi rx: ");
    uart_put_hex32(rx);
    uart_puts("\r\n");

    if (rx == 0xCAFEF00DUL) {
        uart_puts("spi PASS\r\n");
    } else {
        uart_puts("spi FAIL\r\n");
    }
    while (1) { __asm__ volatile ("wfi"); }
    return 0;
}
