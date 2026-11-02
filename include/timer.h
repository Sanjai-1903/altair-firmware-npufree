#ifndef TIMER_H
#define TIMER_H

/*
 * timer.h - Timer driver aligned to current RTL
 * No stdlib / POSIX headers.
 *
 * Verified RTL contract:
 *   TIMER_VALUE         0x00 read-only current counter
 *   TIMER_COMPARE       0x04 compare register
 *   TIMER_CONTROL       0x08 bit 0 = periodic mode
 *                              bit 1 = one-shot mode
 *   TIMER_PRESCALER     0x0C prescale threshold
 *   TIMER_STATUS        0x10 bit 0 = match flag
 *   TIMER_INTERRUPT_CLR 0x14 write clears match flag
 *
 * Important RTL limitations:
 *   - no explicit enable bit
 *   - no explicit start/stop bit
 *   - TIMER_VALUE is not writable
 *   - STATUS bit 1 running is not implemented
 *   - prescaler divide ratio is (PRESCALER + 1)
 */

#include <stdint.h>
#include "soc.h"

#define TIMER_VALUE         0x00U
#define TIMER_COMPARE       0x04U
#define TIMER_CONTROL       0x08U
#define TIMER_PRESCALER     0x0CU
#define TIMER_STATUS        0x10U
#define TIMER_INTERRUPT_CLR 0x14U

#define TIMER_CTRL_PERIODIC (1U << 0)
#define TIMER_CTRL_ONE_SHOT (1U << 1)

#define TIMER_STAT_MATCH    (1U << 0)

#define TIMER_REG(offset)   REG(TIMER_BASE, offset)

void timer_init(uint32_t prescaler);
/* Compatibility shims: current RTL starts counting after reset and has no stop gate. */
void timer_start(void);
void timer_stop(void);
void timer_set_compare(uint32_t cmp);
uint32_t timer_get_value(void);
void timer_delay_ms(uint32_t ms);
void timer_irq_enable(void);
void timer_irq_clear(void);
uint32_t timer_irq_status(void);

/*
 * INTC-driven interrupt path (trap.c:timer_irq_service). Call
 * intc_enable(IRQ_TIMER) + intc_global_enable() separately, matching the
 * DMA/SNN driver pattern — timer_irq_enable() above only arms the
 * peripheral-local match/periodic mode, not the INTC.
 */
void timer_irq_flag_clear(void);
int timer_irq_wait(void);

/* timer_get_cycles() - read the Ibex mcycle CSR.
 * FW_HOST_BUILD has no CSRs; the host device model supplies a monotonic
 * cycle count instead so latency accounting can be exercised natively.
 * See host/README.md. */
#ifdef FW_HOST_BUILD
uint32_t fw_mock_cycles(void);
static inline uint32_t timer_get_cycles(void)
{
    return fw_mock_cycles();
}
#else
static inline uint32_t timer_get_cycles(void)
{
    uint32_t v;
    __asm__ volatile ("csrr %0, mcycle" : "=r"(v) :: "memory");
    return v;
}
#endif

void watchdog_init(uint32_t timeout_ms);
void watchdog_kick(void);
int watchdog_expired(void);

#endif /* TIMER_H */
