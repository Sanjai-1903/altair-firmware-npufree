#ifndef INTC_H
#define INTC_H

/*
 * intc.h - Interrupt controller driver aligned to current RTL
 * Ibex M-mode only. No S-mode CSRs touched.
 * No stdlib / POSIX headers.
 *
 * Verified RTL contract:
 *   INTC_PENDING  0x00 read latched raw pending bits
 *   INTC_ENABLE   0x04 read/write interrupt mask bits
 *   INTC_PRIORITY 0x08 read/write 8 packed 4-bit priorities
 *   INTC_VECTOR   0x0C read-only one-hot highest-priority enabled pending IRQ
 *   INTC_STATUS   0x10 read-only alias of INTC_VECTOR
 *   INTC_ACK      0x14 write-1-to-clear pending bits
 *
 * CPU integration:
 *   - INTC drives Ibex `irq_external_i`
 *   - firmware must enable machine external interrupts in `mie.MEIE`
 *   - firmware must also enable global interrupts with `mstatus.MIE`
 */

#include <stdint.h>
#include "soc.h"

#define INTC_PENDING        0x00U
#define INTC_ENABLE         0x04U
#define INTC_PRIORITY       0x08U
#define INTC_VECTOR         0x0CU
#define INTC_STATUS         0x10U
#define INTC_ACK            0x14U

/*
 * RTL has 8 irq_inputs: DMA(0), TIMER(1), UART(2), SPI(3), I2C(4),
 * GPIO(5), <unconnected>(6), SNN(7).
 *
 * NPU-FREE BUILD: input 6 was the NPU completion interrupt. With the NPU
 * block removed the line is tied low, so bit 6 can never become pending.
 * There is deliberately no IRQ_NPU constant any more -- enabling it would
 * mask in a source that cannot fire, and the ISR has no handler for it
 * (see src/trap.c). Do not reuse bit 6 for a new peripheral without
 * updating the INTC priority word packing below, which is positional.
 */
#define IRQ_DMA             (1U << 0)
#define IRQ_TIMER           (1U << 1)
#define IRQ_UART            (1U << 2)
#define IRQ_SPI             (1U << 3)
#define IRQ_I2C             (1U << 4)
#define IRQ_GPIO            (1U << 5)
/* bit 6: unconnected -- formerly IRQ_NPU */
#define IRQ_SNN             (1U << 7)

#define INTC_REG(offset)    REG(INTC_BASE, offset)

void intc_init(void);
void intc_enable(uint32_t mask);
void intc_disable(uint32_t mask);
void intc_set_priority(uint32_t priority_word);
uint32_t intc_get_pending(void);
void intc_ack(uint32_t irq_mask);
uint32_t intc_status(void);
void intc_global_enable(void);
void intc_global_disable(void);

#endif /* INTC_H */
