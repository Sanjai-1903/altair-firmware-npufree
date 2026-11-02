/*
 * mock_soc.c — host-side device model. TEST FIXTURE ONLY; see mock_soc.h.
 *
 * Implements just enough of each peripheral for the real drivers in src/
 * to run natively:
 *   UART   TX never busy; every TX write is captured for the telemetry
 *          decoder in test_host.c. RX is not modelled at all -- the sensor
 *          byte path is replaced wholesale by host/mock_sensor.c, which is
 *          the layer sensor.h already documents as the one to swap for
 *          bring-up.
 *   TIMER  VALUE is a monotonic counter that advances on every read, so
 *          timer_delay_ms()'s compare loop terminates and watchdog
 *          arithmetic has something to work with.
 *   INTC   plain registers; no interrupt is ever delivered, because there
 *          is no trap handler in a host build.
 *   SNN    a deliberately simple LIF stand-in (see snn_run below).
 *   others plain read/write registers.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "mock_soc.h"
#include "soc.h"
#include "snn.h"
#include "uart.h"
#include "timer.h"

/* ── ISR flag storage ──────────────────────────────────────────────────────
 * On the target these live in src/trap.c, which is not part of a host
 * build (it is the machine-mode trap handler). The drivers reference them
 * as externs, so they are defined here and stay zero: no interrupt is
 * delivered in a host build, and every driver has a polling path.
 * ───────────────────────────────────────────────────────────────────────── */
volatile uint32_t _dma_irq_done;
volatile uint32_t _dma_irq_err;
volatile uint32_t _snn_irq_done;
volatile uint32_t _snn_irq_err;
volatile uint32_t _timer_irq_done;
volatile uint32_t _gpio_irq_done;
volatile uint32_t _gpio_irq_status;
volatile uint32_t _uart_irq_rx_done;
volatile uint32_t _uart_irq_tx_done;
volatile uint32_t _uart_rx_byte;
volatile uint32_t _spi_irq_done;
volatile uint32_t _i2c_irq_done;

/* ── Backing store ─────────────────────────────────────────────────────────
 * A flat page per peripheral, indexed by (addr - base). 4 KiB covers every
 * window this firmware touches, including the SNN's synapse (0x100) and
 * input-buffer (0x200) windows.
 * ───────────────────────────────────────────────────────────────────────── */
#define PAGE_BYTES   0x1000U
#define NUM_PAGES    8U

static const uint32_t s_bases[NUM_PAGES] = {
    SNN_BASE, DMA_BASE, TIMER_BASE, INTC_BASE,
    UART_BASE, SPI_BASE, I2C_BASE, GPIO_BASE,
};
/* PWM shares the generic path via the fallback page below. */
static uint8_t s_page[NUM_PAGES][PAGE_BYTES];
static uint8_t s_fallback[PAGE_BYTES];

/* DMA has four 0x40-byte channel blocks inside its page, so the single
 * DMA page above is enough; nothing else needs more than 0x1000. */

static uint8_t *resolve(uint32_t addr, uint32_t *base_out, uint32_t *off_out)
{
    uint32_t i;

    for (i = 0U; i < NUM_PAGES; i++) {
        if (addr >= s_bases[i] && addr < s_bases[i] + PAGE_BYTES) {
            *base_out = s_bases[i];
            *off_out  = addr - s_bases[i];
            return s_page[i];
        }
    }
    if (addr >= PWM_BASE && addr < PWM_BASE + PAGE_BYTES) {
        *base_out = PWM_BASE;
        *off_out  = addr - PWM_BASE;
        return s_fallback;
    }
    /*
     * Anything else is a firmware bug this fixture should make loud, not
     * absorb -- most importantly a read or write at 0x2000_0000, the
     * address range the removed NPU used to occupy, which must stay
     * unmapped. Abort rather than silently aliasing it somewhere.
     */
    fprintf(stderr,
            "mock_soc: access to unmapped address 0x%08X "
            "(0x20000000..0x2000FFFF is the removed NPU window)\n",
            addr);
    abort();
}

/* ── UART TX capture ───────────────────────────────────────────────────── */
#define TX_CAP_BYTES  65536U
static uint8_t  s_tx[TX_CAP_BYTES];
static size_t   s_tx_len;

void mock_soc_sync(void);   /* defined below with the access hooks */

void mock_uart_tx_reset(void)          { mock_soc_sync(); s_tx_len = 0U; }
size_t mock_uart_tx_len(void)          { mock_soc_sync(); return s_tx_len; }
const uint8_t *mock_uart_tx_data(void) { mock_soc_sync(); return s_tx; }

/* ── Timer ─────────────────────────────────────────────────────────────── */
static uint32_t s_cycles;

uint32_t fw_mock_cycles(void)
{
    /*
     * Monotonic, and deliberately coarse: one "inference" worth of cycles
     * per call pair, so latency_update() sees a plausible non-zero
     * microsecond figure. A host build cannot measure the target's real
     * cycle count, and must not be read as if it could.
     */
    s_cycles += 50000U;   /* 1 ms at 50 MHz */
    return s_cycles;
}

/* ── SNN ───────────────────────────────────────────────────────────────── */
static uint32_t s_snn_spikes;
static uint32_t s_snn_launches;

uint32_t mock_snn_last_spikes(void)  { return s_snn_spikes; }
uint32_t mock_snn_launch_count(void) { return s_snn_launches; }

/*
 * snn_run() - LIF stand-in.
 *
 * NOT a model of neuron_core.sv. It integrates each neuron's INT8
 * external stimulus (input_buffer, written by snn_write_input()) with a
 * per-timestep leak, fires on >= threshold with a subtractive reset, and
 * reports the final timestep's spike vector. The synapse / connect_mask
 * self-feedback path is applied as a single scalar weight per neuron,
 * matching only the shape of the register contract in snn.h, not its
 * behaviour. It exists so inference_run() has a device that completes and
 * returns a decision.
 */
static void snn_run(uint8_t *page)
{
    const uint8_t *in_buf  = page + SNN_INPUT_BUFFER_WINDOW;
    const uint8_t *syn_mem = page + SNN_SYNAPSE_WINDOW;
    uint32_t threshold, leak, timesteps, mask, t, n;
    int32_t  state[32];
    uint32_t spiked_prev = 0U, spiked = 0U;

    memcpy(&threshold, page + SNN_THRESHOLD, 4);
    memcpy(&leak,      page + SNN_LEAK, 4);
    memcpy(&timesteps, page + SNN_TIMESTEPS, 4);
    memcpy(&mask,      page + SNN_CONNECT_MASK_0, 4);

    for (n = 0U; n < 32U; n++) {
        state[n] = 0;
    }
    if (timesteps == 0U) {
        timesteps = 1U;
    }

    for (t = 0U; t < timesteps; t++) {
        spiked = 0U;
        for (n = 0U; n < 32U; n++) {
            int32_t ext = (int32_t)(int8_t)in_buf[n];
            int32_t fb  = 0;

            if ((mask >> n) & 1U) {
                if ((spiked_prev >> n) & 1U) {
                    fb = (int32_t)(int8_t)syn_mem[n];
                }
            }
            state[n] += ext + fb - (int32_t)leak;
            if (state[n] < 0) {
                state[n] = 0;
            }
            if (state[n] >= (int32_t)threshold) {
                state[n] -= (int32_t)threshold;
                spiked |= (1U << n);
            }
        }
        spiked_prev = spiked;
    }

    s_snn_spikes = spiked;
    s_snn_launches++;

    /* Publish results the way the register contract does: spike vector in
     * SNN_SPIKE_OUT_0, DONE sticky, BUSY clear. */
    memcpy(page + SNN_SPIKE_OUT_0, &spiked, 4);
    {
        uint32_t status = SNN_STAT_DONE | SNN_STAT_INIT_DONE;
        memcpy(page + SNN_STATUS, &status, 4);
        uint32_t ctrl = 0U;   /* START self-clears on the busy-falling edge */
        memcpy(page + SNN_CONTROL, &ctrl, 4);
    }
}

/* ── Access hooks ──────────────────────────────────────────────────────────
 * How a write is detected. REG() expands to a plain dereference, so the
 * backend below cannot see a store happen; it only hands out a pointer.
 * It therefore snapshots the word it is about to hand out, and on the NEXT
 * access compares the stored word against that snapshot -- a difference
 * means the firmware stored something. After servicing a write-triggered
 * register, the handler parks a sentinel value in it that no legitimate
 * write can equal, so a second identical write (two 'l's in a row through
 * UART_TX, two back-to-back STARTs) is still seen as a change. mock_soc_
 * sync() forces that comparison, and the UART capture accessors call it,
 * so the last byte written before an inspection is never missed.
 *
 * This is the one genuinely delicate thing in the fixture. It is reliable
 * for the write-then-other-access patterns every driver in src/ uses, and
 * it is why the tests assert on captured UART output and on returned
 * values rather than on register contents.
 * ───────────────────────────────────────────────────────────────────────── */
#define WRITE_ONLY_SENTINEL  0xDEAD5E47UL

static void park(uint8_t *page, uint32_t off)
{
    uint32_t sentinel = WRITE_ONLY_SENTINEL;
    memcpy(page + off, &sentinel, 4);
}

static void on_write(uint32_t base, uint32_t off, uint8_t *page, uint32_t val)
{
    if (base == UART_BASE && off == UART_TX) {
        if (s_tx_len < TX_CAP_BYTES) {
            s_tx[s_tx_len++] = (uint8_t)(val & 0xFFU);
        }
        park(page, off);
        return;
    }
    if (base == SNN_BASE && off == SNN_CONTROL) {
        if (val & SNN_CTRL_START) {
            snn_run(page);
        }
        return;
    }
    if (base == SNN_BASE && off == SNN_INTERRUPT_CLR) {
        park(page, off);
        return;
    }
    if (base == TIMER_BASE && off == TIMER_INTERRUPT_CLR) {
        uint32_t status = 0U;
        memcpy(page + TIMER_STATUS, &status, 4);
        park(page, off);
        return;
    }
}

static void refresh_on_read(uint32_t base, uint32_t off, uint8_t *page)
{
    if (base == UART_BASE && off == UART_STATUS) {
        uint32_t status = 0U;        /* TX never busy */
        memcpy(page + UART_STATUS, &status, 4);
        return;
    }
    if (base == TIMER_BASE && (off == TIMER_VALUE || off == TIMER_STATUS)) {
        uint32_t value, compare, status;

        /* VALUE advances on every look at either register, so
         * timer_delay_ms()'s compare loop and the watchdog both make
         * progress. The step size is arbitrary -- a host build cannot
         * represent the target's 50 MHz tick. */
        memcpy(&value, page + TIMER_VALUE, 4);
        value += 1000U;
        memcpy(page + TIMER_VALUE, &value, 4);

        memcpy(&compare, page + TIMER_COMPARE, 4);
        memcpy(&status,  page + TIMER_STATUS, 4);
        if (compare != 0U && value >= compare) {
            status |= TIMER_STAT_MATCH;
            memcpy(page + TIMER_STATUS, &status, 4);
        }
        return;
    }
}

static uint32_t s_shadow_val;
static uint8_t *s_shadow_page;
static uint32_t s_shadow_base, s_shadow_off;
static int      s_shadow_valid;

static void flush_shadow(void)
{
    if (s_shadow_valid) {
        uint32_t now;

        s_shadow_valid = 0;
        memcpy(&now, s_shadow_page + s_shadow_off, 4);
        if (now != s_shadow_val) {
            on_write(s_shadow_base, s_shadow_off, s_shadow_page, now);
        }
    }
}

void mock_soc_sync(void)
{
    flush_shadow();
}

volatile uint32_t *fw_mock_word(uint32_t addr)
{
    uint32_t base, off;
    uint8_t *page;

    flush_shadow();
    page = resolve(addr, &base, &off);
    refresh_on_read(base, off, page);

    s_shadow_page  = page;
    s_shadow_base  = base;
    s_shadow_off   = off;
    memcpy(&s_shadow_val, page + off, 4);
    s_shadow_valid = 1;

    return (volatile uint32_t *)(void *)(page + off);
}

volatile uint8_t *fw_mock_byte(uint32_t addr)
{
    uint32_t base, off;
    uint8_t *page;

    flush_shadow();
    page = resolve(addr, &base, &off);
    return (volatile uint8_t *)(page + off);
}

void mock_soc_reset(void)
{
    memset(s_page, 0, sizeof s_page);
    memset(s_fallback, 0, sizeof s_fallback);
    s_tx_len = 0U;
    s_cycles = 0U;
    s_snn_spikes = 0U;
    s_snn_launches = 0U;
    s_shadow_valid = 0;
    _snn_irq_done = 0U;
    _snn_irq_err  = 0U;
    _uart_irq_rx_done = 0U;
}
