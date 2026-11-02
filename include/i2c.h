#ifndef I2C_H
#define I2C_H

/*
 * i2c.h — I2C Master driver
 *
 * RTL constraints (i2c_master.sv, i2c_top.sv):
 * - Fixed transaction sequence triggered by writing to TX register:
 *   START -> 8-bit TX -> ACK check -> 8-bit RX -> Master NACK -> STOP.
 * - Cannot separate START, address, data writes, and STOP.
 * - I2C_TX (0x00): Write triggers fixed transaction.
 * - I2C_RX (0x04): Contains the byte received during the RX phase.
 * - I2C_STATUS (0x08): Bit 0 = busy, Bit 1 = NACK error during TX phase.
 * - I2C_CONTROL (0x0C): Bit 0 = IRQ enable on done.
 * - I2C_CLKDIV (0x10): Clock divider.
 *
 * Hardware Limitations:
 * - Single-byte write + single-byte read only.
 * - No standard repeated-start or arbitrary write/read streams.
 * - No clock stretching support.
 */

#include <stdint.h>
#include "soc.h"

#define I2C_REG(offset)    REG(I2C_BASE, offset)

#define I2C_REG_TX         0x00U
#define I2C_REG_RX         0x04U
#define I2C_REG_STATUS     0x08U
#define I2C_REG_CONTROL    0x0CU
#define I2C_REG_CLKDIV     0x10U

void i2c_init(uint32_t clkdiv);
uint8_t i2c_transfer(uint8_t tx_byte, int *nack_err);
int i2c_busy(void);

/*
 * INTC-driven interrupt path (trap.c:i2c_irq_service). Call
 * intc_enable(IRQ_I2C) + intc_global_enable() separately.
 */
void i2c_irq_enable(void);    /* I2C_CONTROL[0] = 1 (done-IRQ enable) */
void i2c_irq_disable(void);   /* I2C_CONTROL = 0 */
void i2c_irq_flag_clear(void);
int i2c_irq_wait(void);

#endif /* I2C_H */
