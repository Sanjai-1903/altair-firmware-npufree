/*
 * gpio.c - GPIO driver aligned to current RTL
 * No stdlib / POSIX headers.
 */

#include "gpio.h"

/* ISR flags defined in trap.c; written only from M-mode trap context */
extern volatile uint32_t _gpio_irq_done;
extern volatile uint32_t _gpio_irq_status;

/*
 * RTL does not expose output-latch readback. Keep a software shadow so
 * masked writes and toggles target the output register rather than live pins.
 */
static uint8_t g_gpio_data_shadow;

void gpio_init(void)
{
    g_gpio_data_shadow = 0U;
    GPIO_REG(GPIO_DATA) = 0U;
    GPIO_REG(GPIO_DIRECTION) = 0U;
    GPIO_REG(GPIO_INTERRUPT_ENABLE) = 0U;
}

void gpio_set_output(uint8_t mask)
{
    GPIO_REG(GPIO_DIRECTION) |= (uint32_t)mask;
}

void gpio_set_input(uint8_t mask)
{
    GPIO_REG(GPIO_DIRECTION) &= ~(uint32_t)mask;
}

void gpio_write(uint8_t mask, uint8_t val)
{
    g_gpio_data_shadow =
        (uint8_t)((g_gpio_data_shadow & (uint8_t)~mask) | (val & mask));
    GPIO_REG(GPIO_DATA) = (uint32_t)g_gpio_data_shadow;
}

uint8_t gpio_read(void)
{
    return (uint8_t)(GPIO_REG(GPIO_DATA) & 0xFFU);
}

void gpio_set(uint8_t mask)
{
    g_gpio_data_shadow = (uint8_t)(g_gpio_data_shadow | mask);
    GPIO_REG(GPIO_DATA) = (uint32_t)g_gpio_data_shadow;
}

void gpio_clear(uint8_t mask)
{
    g_gpio_data_shadow = (uint8_t)(g_gpio_data_shadow & (uint8_t)~mask);
    GPIO_REG(GPIO_DATA) = (uint32_t)g_gpio_data_shadow;
}

void gpio_toggle(uint8_t mask)
{
    g_gpio_data_shadow = (uint8_t)(g_gpio_data_shadow ^ mask);
    GPIO_REG(GPIO_DATA) = (uint32_t)g_gpio_data_shadow;
}

void gpio_irq_enable(uint8_t mask)
{
    GPIO_REG(GPIO_INTERRUPT_ENABLE) |= (uint32_t)mask;
}

void gpio_irq_clear(uint8_t mask)
{
    (void)mask;
    /* RTL has no GPIO-local clear register; INTC ack is handled separately. */
}

uint8_t gpio_irq_status(void)
{
    return (uint8_t)(GPIO_REG(GPIO_INTERRUPT_STATUS) & 0xFFU);
}

void gpio_irq_flag_clear(void)
{
    _gpio_irq_done = 0U;
}

int gpio_irq_wait(void)
{
    uint32_t timeout = 0x100000UL;   /* ~2M cycles @ 50 MHz ~= 40 ms */

    while (!_gpio_irq_done) {
        if (--timeout == 0U)
            return -1;
    }
    return 0;
}

uint8_t gpio_irq_last_status(void)
{
    return (uint8_t)(_gpio_irq_status & 0xFFU);
}
