#ifndef PWM_H
#define PWM_H

/*
 * pwm.h — PWM controller driver
 * 
 * RTL constraints (pwm_top.sv, pwm_ctrl.sv):
 * - 4 independent channels sharing a single period.
 * - PWM_PERIOD (0x00)
 * - PWM_DUTY_0 (0x04)
 * - PWM_DUTY_1 (0x08)
 * - PWM_DUTY_2 (0x0C)
 * - PWM_DUTY_3 (0x10)
 * - PWM_CONTROL (0x14): Bit 0 enables/disables PWM.
 * - PWM_STATUS (0x18): Bit 0 reflects enable state.
 * 
 * Hardware Limitations:
 * - No interrupt generation.
 * - If DUTY >= PERIOD, output is continuously high.
 */

#include <stdint.h>
#include "soc.h"

#define PWM_REG(offset)    REG(PWM_BASE, offset)

#define PWM_REG_PERIOD     0x00U
#define PWM_REG_DUTY_0     0x04U
#define PWM_REG_DUTY_1     0x08U
#define PWM_REG_DUTY_2     0x0CU
#define PWM_REG_DUTY_3     0x10U
#define PWM_REG_CONTROL    0x14U
#define PWM_REG_STATUS     0x18U

void pwm_init(void);
void pwm_enable(void);
void pwm_disable(void);
void pwm_set_period(uint32_t period);
uint32_t pwm_get_period(void);
void pwm_set_duty(uint8_t channel, uint32_t duty);
uint32_t pwm_get_duty(uint8_t channel);

#endif /* PWM_H */
