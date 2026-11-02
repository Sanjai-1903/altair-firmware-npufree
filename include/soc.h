#ifndef SOC_H
#define SOC_H

/*
 * soc.h — Memory map and register access for the ALTair firmware
 * CPU  : Ibex RV32IM, 50 MHz, bare-metal
 * Mode : M-mode only, no OS, no stdlib, no POSIX
 *
 * NPU-FREE BUILD. The NPU block (formerly NPU_BASE = 0x2000_0000, with its
 * weight SRAM / activation SRAM / accumulator windows at 0x2000_1000,
 * 0x2000_2000 and 0x2000_3000) is not part of this SoC. Nothing in this
 * firmware reads or writes that address range, and 0x2000_0000 is left
 * UNMAPPED on purpose: a stray access there must raise a bus error rather
 * than silently alias some other peripheral. The convolution work the NPU
 * used to do is executed on the CPU instead — see include/conv1d.h.
 *
 * The on-chip neural accelerator in this build is the SNN alone
 * (SNN_BASE), which is also what the ALTair GF180 architecture document's
 * logical memory map specifies.
 *
 * Deltas against that GF180 map, kept deliberately: this firmware targets
 * the RTL as actually built, which additionally has DMA (0x2200_0000),
 * I2C (0x4200_0000) and PWM (0x4400_0000), and whose telemetry/boot-state
 * register blocks (GF180 0x2300_0000 / 0x2400_0000) do not exist yet.
 * See docs/NPU_REMOVAL.md.
 */

#include <stdint.h>

/* ─── Peripheral base addresses ─────────────────────────────────────────── */
#define IRAM_BASE        0x00000000UL
#define DRAM_BASE        0x10000000UL
/* 0x20000000 — formerly NPU. UNMAPPED in this build; do not reuse. */
#define SNN_BASE         0x21000000UL
#define DMA_BASE         0x22000000UL
#define TIMER_BASE       0x30000000UL
#define INTC_BASE        0x31000000UL
#define UART_BASE        0x40000000UL
#define SPI_BASE         0x41000000UL
#define I2C_BASE         0x42000000UL
#define GPIO_BASE        0x43000000UL
#define PWM_BASE         0x44000000UL
#define DEBUG_BASE       0x50000000UL

/* ─── System clock ───────────────────────────────────────────────────────── */
#define SYS_CLK_HZ       50000000UL

/* ─── Register access ───────────────────────────────────────────────────────
 * On the target, REG() casts an absolute address to a volatile 32-bit
 * pointer; SOC_BYTE_PORT() does the same for the byte-addressable SNN
 * synapse / input-buffer windows (see snn.h), which are indexed by neuron
 * number rather than by a fixed register offset.
 *
 * FW_HOST_BUILD routes both through the host device model in host/, so the
 * same driver sources can be compiled and exercised natively. The host
 * model is a test fixture only — it is NOT a verified model of the RTL and
 * proves nothing about hardware behaviour. See host/README.md.
 * ─────────────────────────────────────────────────────────────────────────── */
#ifdef FW_HOST_BUILD

volatile uint32_t *fw_mock_word(uint32_t addr);
volatile uint8_t  *fw_mock_byte(uint32_t addr);

#define REG(base, off)        (*fw_mock_word((uint32_t)(base) + (uint32_t)(off)))
#define SOC_BYTE_PORT(addr)   fw_mock_byte((uint32_t)(addr))

#else

/* Cast base to uint32_t explicitly to avoid pointer-arithmetic UB on RV32.
 * The outer cast to (volatile uint32_t *) is safe because peripheral
 * registers are always 32-bit aligned on this SoC. */
#define REG(base, off) \
    (*((volatile uint32_t *)((uint32_t)(base) + (off))))

#define SOC_BYTE_PORT(addr)   ((volatile uint8_t *)(uint32_t)(addr))

#endif /* FW_HOST_BUILD */

#endif /* SOC_H */
