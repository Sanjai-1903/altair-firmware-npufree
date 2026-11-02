/*
 * test_pwm.c — PWM controller driver / RTL test
 * Links as a standalone ELF with its own main().
 * No stdlib, no printf.
 *
 * Co-designed with SoC/RTL_FIRMWARE/tests/test_pwm.py: the cocotb bench
 * measures the pwm_o waveform during the enabled window (period=1000,
 * duties 250/500/750, plus duty>period on channel 3 to exercise the
 * documented continuous-high case), then verifies all outputs drop after
 * pwm_disable(). This image self-checks the register readback and
 * STATUS-follows-enable contract and reports over UART.
 */

#include "uart.h"
#include "pwm.h"

int main(void)
{
    uint32_t p, d0, s_en, s_dis;

    uart_init(115200U);
    uart_puts("pwm test start\r\n");

    pwm_init();
    pwm_set_period(1000U);
    pwm_set_duty(0U, 250U);   /* 25% */
    pwm_set_duty(1U, 500U);   /* 50% */
    pwm_set_duty(2U, 750U);   /* 75% */
    pwm_set_duty(3U, 1500U);  /* > period: documented continuous-high case */

    pwm_enable();
    uart_puts("pwm enabled\r\n");

    /* Readback + status while enabled */
    p    = pwm_get_period();
    d0   = pwm_get_duty(0U);
    s_en = PWM_REG(PWM_REG_STATUS);

    /* Hold the enabled window open long enough for the bench to measure
     * several full 1000-cycle periods (~15k cycles ≈ 0.3 ms @ 50 MHz;
     * the UART prints above add several ms on top of that). */
    {
        volatile uint32_t d = 3000U;
        while (d--) { /* nothing */ }
    }

    pwm_disable();
    s_dis = PWM_REG(PWM_REG_STATUS);
    uart_puts("pwm disabled\r\n");

    if (p == 1000U && d0 == 250U && (s_en & 1U) == 1U && (s_dis & 1U) == 0U) {
        uart_puts("pwm PASS\r\n");
    } else {
        uart_puts("pwm FAIL\r\n");
    }
    while (1) { __asm__ volatile ("wfi"); }
    return 0;
}
