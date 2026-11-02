/*
 * intc.c - Interrupt controller driver aligned to current RTL
 * Ibex M-mode only. No stdlib / POSIX headers.
 */

#include "intc.h"

#define MIE_MEIE_MASK       (1U << 11)

/*
 * mstatus.MIE / mie.MEIE are Ibex CSRs, written with CSR instructions.
 * FW_HOST_BUILD has no CSRs and delivers no interrupts, so these become
 * no-ops there and every driver takes its polling path -- see
 * host/README.md. The target build is unaffected.
 */
#ifdef FW_HOST_BUILD

static inline void _mstatus_mie_set(void)   { }
static inline void _mstatus_mie_clear(void) { }
static inline void _mie_meie_set(void)      { }
static inline void _mie_meie_clear(void)    { }

#else

static inline void _mstatus_mie_set(void)
{
    __asm__ volatile ("csrsi mstatus, 0x8" ::: "memory");
}

static inline void _mstatus_mie_clear(void)
{
    __asm__ volatile ("csrci mstatus, 0x8" ::: "memory");
}

static inline void _mie_meie_set(void)
{
    __asm__ volatile ("csrs mie, %0" :: "r"(MIE_MEIE_MASK) : "memory");
}

static inline void _mie_meie_clear(void)
{
    __asm__ volatile ("csrc mie, %0" :: "r"(MIE_MEIE_MASK) : "memory");
}

#endif /* FW_HOST_BUILD */

void intc_init(void)
{
    _mstatus_mie_clear();
    _mie_meie_clear();

    INTC_REG(INTC_ENABLE) = 0U;
    INTC_REG(INTC_ACK) = 0xFFU;
    INTC_REG(INTC_PRIORITY) = 0U;
}

void intc_enable(uint32_t mask)
{
    INTC_REG(INTC_ENABLE) |= mask;
}

void intc_disable(uint32_t mask)
{
    INTC_REG(INTC_ENABLE) &= ~mask;
}

void intc_set_priority(uint32_t priority_word)
{
    INTC_REG(INTC_PRIORITY) = priority_word;
}

uint32_t intc_get_pending(void)
{
    return INTC_REG(INTC_PENDING);
}

void intc_ack(uint32_t irq_mask)
{
    INTC_REG(INTC_ACK) = irq_mask;
}

uint32_t intc_status(void)
{
    return INTC_REG(INTC_STATUS);
}

void intc_global_enable(void)
{
    _mie_meie_set();
    _mstatus_mie_set();
}

void intc_global_disable(void)
{
    _mstatus_mie_clear();
    _mie_meie_clear();
}
