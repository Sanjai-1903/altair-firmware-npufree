#include "spi.h"

/* ISR completion flag defined in trap.c; written only from M-mode trap context */
extern volatile uint32_t _spi_irq_done;

void spi_init(uint32_t clkdiv) {
    SPI_REG(SPI_REG_CONTROL) = 0; // Disable IRQ by default
    SPI_REG(SPI_REG_CLKDIV) = clkdiv;
}

int spi_busy(void) {
    return (SPI_REG(SPI_REG_STATUS) & 1);
}

uint32_t spi_transfer(uint32_t tx_data) {
    SPI_REG(SPI_REG_TX) = tx_data;
    while (spi_busy()) {
        // Wait for hardware transaction to complete
    }
    return SPI_REG(SPI_REG_RX);
}

void spi_irq_enable(void) {
    SPI_REG(SPI_REG_CONTROL) = 1U;
}

void spi_irq_disable(void) {
    SPI_REG(SPI_REG_CONTROL) = 0U;
}

void spi_irq_flag_clear(void) {
    _spi_irq_done = 0U;
}

int spi_irq_wait(void) {
    uint32_t timeout = 0x100000UL;   /* ~2M cycles @ 50 MHz ~= 40 ms */

    while (!_spi_irq_done) {
        if (--timeout == 0U)
            return -1;
    }
    return 0;
}
