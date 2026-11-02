#include "i2c.h"

/* ISR completion flag defined in trap.c; written only from M-mode trap context */
extern volatile uint32_t _i2c_irq_done;

void i2c_init(uint32_t clkdiv) {
    I2C_REG(I2C_REG_CONTROL) = 0; // Disable IRQ by default
    I2C_REG(I2C_REG_CLKDIV) = clkdiv;
}

int i2c_busy(void) {
    return (I2C_REG(I2C_REG_STATUS) & 1);
}

uint8_t i2c_transfer(uint8_t tx_byte, int *nack_err) {
    I2C_REG(I2C_REG_TX) = tx_byte;
    while (i2c_busy()) {
        // Wait for fixed transaction to complete
    }
    
    if (nack_err) {
        *nack_err = (I2C_REG(I2C_REG_STATUS) & 2) ? 1 : 0;
    }
    
    return (uint8_t)(I2C_REG(I2C_REG_RX) & 0xFF);
}

void i2c_irq_enable(void) {
    I2C_REG(I2C_REG_CONTROL) = 1U;
}

void i2c_irq_disable(void) {
    I2C_REG(I2C_REG_CONTROL) = 0U;
}

void i2c_irq_flag_clear(void) {
    _i2c_irq_done = 0U;
}

int i2c_irq_wait(void) {
    uint32_t timeout = 0x100000UL;   /* ~2M cycles @ 50 MHz ~= 40 ms */

    while (!_i2c_irq_done) {
        if (--timeout == 0U)
            return -1;
    }
    return 0;
}
