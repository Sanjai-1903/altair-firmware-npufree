#ifndef SPI_H
#define SPI_H

/*
 * spi.h — SPI Master driver
 *
 * RTL constraints (spi_master.sv, spi_top.sv):
 * - Fixed 32-bit transfer triggered by writing to TX register.
 * - Hardware handles CS (active-low, asserted during transaction).
 * - SPI_TX (0x00): Write triggers transaction.
 * - SPI_RX (0x04): Read last received data.
 * - SPI_STATUS (0x08): Bit 0 = busy.
 * - SPI_CONTROL (0x0C): Bit 0 = IRQ enable on done.
 * - SPI_CLKDIV (0x10): Clock divider.
 *
 * Hardware Limitations:
 * - Fixed 32-bit transaction size.
 * - No FIFO.
 * - CPOL/CPHA are fixed (equivalent to CPOL=0, CPHA=0).
 */

#include <stdint.h>
#include "soc.h"

#define SPI_REG(offset)    REG(SPI_BASE, offset)

#define SPI_REG_TX         0x00U
#define SPI_REG_RX         0x04U
#define SPI_REG_STATUS     0x08U
#define SPI_REG_CONTROL    0x0CU
#define SPI_REG_CLKDIV     0x10U

void spi_init(uint32_t clkdiv);
uint32_t spi_transfer(uint32_t tx_data);
int spi_busy(void);

/*
 * INTC-driven interrupt path (trap.c:spi_irq_service). Call
 * intc_enable(IRQ_SPI) + intc_global_enable() separately.
 */
void spi_irq_enable(void);    /* SPI_CONTROL[0] = 1 (done-IRQ enable) */
void spi_irq_disable(void);   /* SPI_CONTROL = 0 */
void spi_irq_flag_clear(void);
int spi_irq_wait(void);

#endif /* SPI_H */
