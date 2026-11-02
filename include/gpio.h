#ifndef GPIO_H
#define GPIO_H

#include <stdint.h>
#include "soc.h"

/*
 * gpio.h - GPIO driver aligned to current RTL
 *
 * Verified RTL contract:
 *   GPIO_DATA             0x00 read current synchronized pin state,
 *                              write output data latch
 *   GPIO_DIRECTION        0x04 bit=1 drives output, bit=0 leaves pin input
 *   GPIO_INTERRUPT_ENABLE 0x08 bit=1 enables mismatch edge detection
 *   GPIO_INTERRUPT_STATUS 0x0C read-only derived mismatch status
 *
 * Important RTL limitations:
 *   - no firmware-visible output-latch readback register
 *   - no GPIO-local interrupt clear register
 *   - interrupt status is combinational from
 *     (pin_state ^ output_latch) & interrupt_enable
 */
#define GPIO_DATA               0x00U
#define GPIO_DIRECTION          0x04U
#define GPIO_INTERRUPT_ENABLE   0x08U
#define GPIO_INTERRUPT_STATUS   0x0CU

#define GPIO_NUM_PINS           8U

#define GPIO_REG(offset)        REG(GPIO_BASE, offset)

#define GPIO_PIN(n)             (1U << (n))

#define GPIO_PIN_HEARTBEAT      GPIO_PIN(0)
#define GPIO_PIN_ANOMALY        GPIO_PIN(1)
#define GPIO_PIN_DEBUG0         GPIO_PIN(2)
#define GPIO_PIN_DEBUG1         GPIO_PIN(3)

void gpio_init(void);
void gpio_set_output(uint8_t mask);
void gpio_set_input(uint8_t mask);
void gpio_write(uint8_t mask, uint8_t val);
uint8_t gpio_read(void);
void gpio_set(uint8_t mask);
void gpio_clear(uint8_t mask);
void gpio_toggle(uint8_t mask);
void gpio_irq_enable(uint8_t mask);
void gpio_irq_clear(uint8_t mask);
uint8_t gpio_irq_status(void);

/*
 * INTC-driven interrupt path (trap.c:gpio_irq_service). Call
 * intc_enable(IRQ_GPIO) + intc_global_enable() separately, matching the
 * DMA/SNN driver pattern.
 */
void gpio_irq_flag_clear(void);
int gpio_irq_wait(void);
/* gpio_irq_last_status() - GPIO_INTERRUPT_STATUS captured by the ISR at
 * the moment of the mismatch pulse (which pin(s) triggered it). */
uint8_t gpio_irq_last_status(void);

#endif /* GPIO_H */
