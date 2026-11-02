#include "pwm.h"

void pwm_init(void) {
    pwm_disable();
    pwm_set_period(0xFFFF);
    pwm_set_duty(0, 0);
    pwm_set_duty(1, 0);
    pwm_set_duty(2, 0);
    pwm_set_duty(3, 0);
}

void pwm_enable(void) {
    PWM_REG(PWM_REG_CONTROL) = 1;
}

void pwm_disable(void) {
    PWM_REG(PWM_REG_CONTROL) = 0;
}

void pwm_set_period(uint32_t period) {
    PWM_REG(PWM_REG_PERIOD) = period;
}

uint32_t pwm_get_period(void) {
    return PWM_REG(PWM_REG_PERIOD);
}

void pwm_set_duty(uint8_t channel, uint32_t duty) {
    switch (channel) {
        case 0: PWM_REG(PWM_REG_DUTY_0) = duty; break;
        case 1: PWM_REG(PWM_REG_DUTY_1) = duty; break;
        case 2: PWM_REG(PWM_REG_DUTY_2) = duty; break;
        case 3: PWM_REG(PWM_REG_DUTY_3) = duty; break;
        default: break; // Ignore invalid channels
    }
}

uint32_t pwm_get_duty(uint8_t channel) {
    switch (channel) {
        case 0: return PWM_REG(PWM_REG_DUTY_0);
        case 1: return PWM_REG(PWM_REG_DUTY_1);
        case 2: return PWM_REG(PWM_REG_DUTY_2);
        case 3: return PWM_REG(PWM_REG_DUTY_3);
        default: return 0;
    }
}
