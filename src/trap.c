/*
 * trap.c — Machine-mode trap handler for Ibex RV32IM
 *
 * mtvec is set to machine_trap_handler (direct mode, mtvec[1:0]=00) by
 * crt0.S before main() runs.  All M-mode exceptions and interrupts land here.
 *
 * DMA interrupt flow:
 *   1. DMA hardware sets STATUS.DONE or STATUS.ERR and asserts the single
 *      combined DMA IRQ line.
 *   2. INTC latches the request in INTC_PENDING (bit IRQ_DMA).
 *   3. CPU jumps to machine_trap_handler (mcause bit 31 set = interrupt).
 *   4. dma_irq_service() scans all 4 DMA STATUS registers, records outcomes
 *      in per-channel flags, then calls intc_ack(IRQ_DMA) once.
 *   5. mret resumes interrupted code; dma_irq_wait() sees the flags.
 *
 * NPU interrupt flow: REMOVED. This build has no NPU block, INTC input 6
 * is tied low (see intc.h), and there is no npu_irq_service() to dispatch
 * to. The convolution work the NPU used to signal completion for now runs
 * to completion synchronously on the CPU inside inference_run(), so it has
 * no interrupt and no completion flag at all -- see conv1d.h.
 *
 * SNN interrupt flow:
 *   1. SNN finishes execution; done pulse sets level-latched irq_o high.
 *   2. INTC latches IRQ_SNN in INTC_PENDING.
 *   3. CPU jumps here; snn_irq_service() reads SNN STATUS, records flags,
 *      writes SNN_INTERRUPT_CLR to clear SNN-side latch, then calls
 *      intc_ack() to clear INTC pending bit.
 *   4. snn_irq_wait() in snn.c polls _snn_irq_done / _snn_irq_err.
 *
 * Timer interrupt flow (RTL: timer_top.sv — timer_irq = TIMER_STATUS[0],
 * a LEVEL signal, not a pulse):
 *   1. Counter reaches TIMER_COMPARE; STATUS.MATCH (bit 0) sets and stays
 *      set (and timer_irq stays high) until firmware clears it.
 *   2. INTC latches IRQ_TIMER.
 *   3. CPU jumps here; timer_irq_service() sets the completion flag, then
 *      writes TIMER_INTERRUPT_CLR (drops the level BEFORE the INTC ack —
 *      same two-stage ordering as NPU/SNN, required here because
 *      IRQ_PENDING re-latches every cycle from the live irq_inputs level;
 *      acking first while timer_irq is still high would just re-set the
 *      pending bit on the very next cycle), then calls intc_ack(IRQ_TIMER).
 *   4. timer_irq_wait() in timer.c polls _timer_irq_done.
 *
 * GPIO interrupt flow (RTL: gpio_top.sv — gpio_irq_o is edge-detected,
 * `mismatch & ~mismatch_q`, already a one-cycle pulse; there is no
 * GPIO-local clear register — RTL_GAPS #8):
 *   1. A masked pin's synchronized input state diverges from GPIO_DATA;
 *      gpio_irq_o pulses once on that transition.
 *   2. INTC latches IRQ_GPIO.
 *   3. CPU jumps here; gpio_irq_service() captures GPIO_INTERRUPT_STATUS
 *      (which pin(s) mismatched) before calling intc_ack(IRQ_GPIO) — no
 *      peripheral-side clear exists or is needed since the source is
 *      already a pulse.
 *   4. gpio_irq_wait() in gpio.c polls _gpio_irq_done.
 *
 * UART interrupt flow (RTL: uart_top.sv — uart_irq_o is a single combined,
 * pulse-based line: `(CONTROL[0] & tx_done_pulse) | (CONTROL[1] & rx_done)`.
 * There is no per-source status bit — the RTL cannot tell software which
 * of the two conditions fired if both are armed simultaneously):
 *   1. A TX completes (CONTROL[0] armed) or a byte is received
 *      (CONTROL[1] armed); uart_irq_o pulses for one cycle.
 *   2. INTC latches IRQ_UART.
 *   3. CPU jumps here; uart_irq_service() inspects UART_CONTROL's own
 *      enable bits to decide which flag(s) to service (this is the only
 *      information the RTL contract offers — see uart.h); if RX is armed
 *      it also captures UART_RX (the RTL holds only the single
 *      most-recently-received byte, no FIFO), then calls intc_ack(IRQ_UART).
 *   4. uart_irq_wait_rx()/uart_irq_wait_tx() in uart.c poll the flags.
 *
 * SPI interrupt flow (RTL: spi_top.sv — spi_irq_o is a one-cycle pulse,
 * `CONTROL[0] & spi_done_pulse`, no peripheral-side clear register):
 *   1. A fixed 32-bit transfer completes; spi_irq_o pulses once.
 *   2. INTC latches IRQ_SPI.
 *   3. CPU jumps here; spi_irq_service() sets the completion flag, then
 *      calls intc_ack(IRQ_SPI) directly (no HW latch to clear first).
 *   4. spi_irq_wait() in spi.c polls _spi_irq_done.
 *
 * I2C interrupt flow (RTL: i2c_top.sv — i2c_irq_o is a one-cycle pulse,
 * `CONTROL[0] & i2c_done_pulse`, no peripheral-side clear register):
 *   1. The fixed START/ADDR/ACK/DATA/NACK/STOP sequence completes
 *      (regardless of ACK/NACK outcome); i2c_irq_o pulses once.
 *   2. INTC latches IRQ_I2C.
 *   3. CPU jumps here; i2c_irq_service() sets the completion flag, then
 *      calls intc_ack(IRQ_I2C) directly (no HW latch to clear first).
 *   4. i2c_irq_wait() in i2c.c polls _i2c_irq_done.
 */

#include "intc.h"
#include "dma.h"
#include "snn.h"
#include "timer.h"
#include "gpio.h"
#include "uart.h"
#include "spi.h"
#include "i2c.h"

/* ── TEMPORARY DEBUG (2026-07-19) ──────────────────────────────────────────
 * Ring buffer of {mepc,mcause,mtval,mstatus} for the first 8 trap entries,
 * filled by crt0.S's _trap_vector before this file's machine_trap_handler
 * prologue runs — see crt0.S for why the capture has to happen there and
 * why 8 entries instead of 1. Layout (4 words/entry) is hand-coded to
 * match _trap_vector's index*16 addressing — do not reorder these fields.
 * Remove alongside the crt0.S block and the dump in machine_trap_handler()
 * once root-caused.
 */
volatile uint32_t dbg_trap_count;
volatile uint32_t dbg_trap_log[8][4];   /* [i][0]=mepc [1]=mcause [2]=mtval [3]=mstatus */
volatile uint32_t dbg_trap_last_dumped;

/* dbg_last_i - legacy breadcrumb from the pre-restoration single-launch
 * inference design (used to bisect a byte-copy loop stall). That specific
 * loop no longer exists (see inference.c), so nothing writes this anymore;
 * it always reads as the sentinel below. Defined HERE, not in inference.c,
 * because trap.c (and this ring-buffer dump) links into every firmware
 * image (production main.c AND every firmware/test/test_*.c image via
 * FW_IMAGE), while inference.c only links into production firmware --
 * defining it there broke every test image's link (undefined reference)
 * the moment this dump started reading it unconditionally. Dumped
 * alongside each ring-buffer entry below so we know which iteration was
 * in-flight (or just-completed) when that specific trap fired. */
volatile uint32_t dbg_last_i = 0xFFFFFFFFU;

/* ── DMA interrupt status flags ──────────────────────────────────────────────
 * Written only from trap context (dma_irq_service).
 * Read from main context via dma_irq_wait() / dma_irq_clear() in dma.c.
 * One bit per channel: bit 0 = CH0, bit 1 = CH1, etc.
 */
volatile uint32_t _dma_irq_done;
volatile uint32_t _dma_irq_err;

/* ── SNN interrupt status flags ──────────────────────────────────────────────
 * Written only from trap context (snn_irq_service).
 * Read from main context via snn_irq_wait() / snn_irq_flag_clear() in snn.c.
 */
volatile uint32_t _snn_irq_done;
volatile uint32_t _snn_irq_err;

/* ── Timer interrupt completion flag ─────────────────────────────────────────
 * Written only from trap context (timer_irq_service) on STATUS.MATCH.
 * Read from main context via timer_irq_wait() in timer.c.
 */
volatile uint32_t _timer_irq_done;

/* ── GPIO interrupt status ───────────────────────────────────────────────────
 * _gpio_irq_status captures GPIO_INTERRUPT_STATUS at the moment of the
 * mismatch pulse (which pin(s) triggered it); written only from trap
 * context (gpio_irq_service). Read via gpio_irq_wait()/gpio_irq_last_status()
 * in gpio.c.
 */
volatile uint32_t _gpio_irq_done;
volatile uint32_t _gpio_irq_status;

/* ── UART interrupt flags ────────────────────────────────────────────────────
 * Written only from trap context (uart_irq_service). _uart_rx_byte holds
 * the last byte captured via the RX-done interrupt (RTL has no FIFO — a
 * byte received before the previous one is read is silently overwritten).
 * Read from main context via uart_irq_wait_rx()/uart_irq_wait_tx()/
 * uart_irq_get_rx_byte() in uart.c.
 */
volatile uint32_t _uart_irq_rx_done;
volatile uint32_t _uart_irq_tx_done;
volatile uint32_t _uart_rx_byte;

/* ── SPI interrupt completion flag ───────────────────────────────────────────
 * Written only from trap context (spi_irq_service). Read via spi_irq_wait()
 * in spi.c.
 */
volatile uint32_t _spi_irq_done;

/* ── I2C interrupt completion flag ───────────────────────────────────────────
 * Written only from trap context (i2c_irq_service). Read via i2c_irq_wait()
 * in i2c.c.
 */
volatile uint32_t _i2c_irq_done;

/* ── DMA ISR ─────────────────────────────────────────────────────────────────
 * RTL has a single combined DMA IRQ line.  When it fires, scan all 4 channels'
 * STATUS registers to find which channel(s) completed or errored, then ack
 * the single IRQ_DMA bit once.
 */
static void dma_irq_service(void)
{
    uint32_t ch;
    for (ch = 0U; ch < DMA_NUM_CHANNELS; ch++) {
        uint32_t st = dma_status(ch);
        if (st & DMA_STAT_DONE) _dma_irq_done |= (1U << ch);
        if (st & DMA_STAT_ERR)  _dma_irq_err  |= (1U << ch);
    }
    intc_ack(IRQ_DMA);
}

/* ── SNN ISR ─────────────────────────────────────────────────────────────────
 * Two-stage clear: SNN hardware latch first, INTC pending second.
 * Same ordering rule as NPU: reversing would allow the INTC to re-latch.
 */
static void snn_irq_service(void)
{
    uint32_t st = SNN_REG(SNN_STATUS);
    /* Record outcome before any clear */
    if (st & SNN_STAT_DONE) _snn_irq_done = 1U;
    /* Stage 1: clear SNN-side interrupt latch (SNN_INTERRUPT_CLR register) */
    snn_irq_clear();
    /* Stage 2: clear INTC pending bit so CPU stops seeing the IRQ */
    intc_ack(IRQ_SNN);
}

/* ── Timer ISR ───────────────────────────────────────────────────────────────
 * timer_irq (=TIMER_STATUS[0]) is a LEVEL signal — it must be dropped via
 * TIMER_INTERRUPT_CLR before intc_ack(), or INTC_PENDING would immediately
 * re-latch it from the still-high irq_inputs bit on the very next cycle.
 */
static void timer_irq_service(void)
{
    _timer_irq_done = 1U;
    /* Stage 1: clear the peripheral-side level (TIMER_STATUS.MATCH) */
    timer_irq_clear();
    /* Stage 2: clear INTC pending bit */
    intc_ack(IRQ_TIMER);
}

/* ── GPIO ISR ────────────────────────────────────────────────────────────────
 * gpio_irq_o is already edge-detected (one-cycle pulse) in RTL, so there is
 * no peripheral-side latch to clear here — capture which pin(s) mismatched
 * before acking so the flag is not lost if GPIO_DATA is written before the
 * waiter observes it.
 */
static void gpio_irq_service(void)
{
    _gpio_irq_status = gpio_irq_status();
    _gpio_irq_done = 1U;
    intc_ack(IRQ_GPIO);
}

/* ── UART ISR ────────────────────────────────────────────────────────────────
 * uart_irq_o combines TX-done and RX-done into a single line with no
 * per-source status bit (RTL_GAPS #6). The only way to know which event(s)
 * are relevant is which CONTROL enable bit(s) software armed — see uart.h.
 */
static void uart_irq_service(void)
{
    uint32_t ctrl = UART_REG(UART_CONTROL);

    if (ctrl & UART_CTRL_IRQ_RX_DONE) {
        _uart_rx_byte = UART_REG(UART_RX);
        _uart_irq_rx_done = 1U;
    }
    if (ctrl & UART_CTRL_IRQ_TX_DONE) {
        _uart_irq_tx_done = 1U;
    }
    intc_ack(IRQ_UART);
}

/* ── SPI ISR ─────────────────────────────────────────────────────────────────
 * spi_irq_o is a one-cycle completion pulse; no peripheral-side latch to
 * clear before the INTC ack.
 */
static void spi_irq_service(void)
{
    _spi_irq_done = 1U;
    intc_ack(IRQ_SPI);
}

/* ── I2C ISR ─────────────────────────────────────────────────────────────────
 * i2c_irq_o is a one-cycle completion pulse (fires regardless of ACK/NACK
 * outcome); no peripheral-side latch to clear before the INTC ack.
 */
static void i2c_irq_service(void)
{
    _i2c_irq_done = 1U;
    intc_ack(IRQ_I2C);
}

/* ── Machine trap handler ────────────────────────────────────────────────────
 * GCC __attribute__((interrupt("machine"))) saves all caller-saved registers,
 * adjusts the stack to 16-byte alignment, and emits mret on return.
 * Direct mode (mtvec[1:0]=00): every trap lands here; mcause distinguishes
 * interrupts (bit 31=1) from synchronous exceptions (bit 31=0).
 */
/* dbg_trap_dump_pending() — mainline-context counterpart of the old
 * in-ISR dump (moved out 2026-07-19). crt0.S's _trap_vector still captures
 * every trap's {mepc,mcause,mtval,mstatus} into dbg_trap_log (cheap register
 * writes, no UART), so this instrumentation is fully preserved for the
 * illegal-instruction investigation. What changed: printing each entry via
 * uart_puts() used to happen INSIDE machine_trap_handler() itself, meaning
 * every trap held off further interrupt servicing for ~6ms (the ring buffer
 * dump is ~70 chars, and this UART has no hardware FIFO -- see uart.c). That
 * measured ~70x longer than the inter-arrival time of a real interrupt
 * source (e.g. UART RX-done at 115200 baud, ~87us/byte), so ANY sustained
 * interrupt source caused the CPU to spend virtually all its time inside
 * the ISR re-entering on newly-queued IRQs, starving mainline code -- this
 * was the actual mechanism behind the "no forward progress" livelocks
 * chased earlier in this investigation (on top of, and independent from,
 * the separate wb_register_slice double-strobe bug). Calling this from
 * main.c's loop instead prints the exact same diagnostic data with zero
 * ISR-side cost. */
void dbg_trap_dump_pending(void)
{
    while (dbg_trap_last_dumped < dbg_trap_count && dbg_trap_last_dumped < 8U) {
        uint32_t i = dbg_trap_last_dumped;
        dbg_trap_last_dumped = i + 1U;
        uart_puts("[dbg] trap[");
        uart_put_uint(i);
        uart_puts("] mepc=");
        uart_put_hex32(dbg_trap_log[i][0]);
        uart_puts(" mcause=");
        uart_put_hex32(dbg_trap_log[i][1]);
        uart_puts(" mtval=");
        uart_put_hex32(dbg_trap_log[i][2]);
        uart_puts(" mstatus=");
        uart_put_hex32(dbg_trap_log[i][3]);
        uart_puts(" last_i=");
        uart_put_uint(dbg_last_i);
        uart_puts("\r\n");
    }
}

__attribute__((interrupt("machine")))
void machine_trap_handler(void)
{
    uint32_t mcause;
    __asm__ volatile ("csrr %0, mcause" : "=r"(mcause));

    if (mcause & (1UL << 31U)) {
        /* External interrupt — query INTC for which source fired */
        uint32_t pending = intc_get_pending();
        if (pending & IRQ_DMA)
            dma_irq_service();
        if (pending & IRQ_TIMER)
            timer_irq_service();
        if (pending & IRQ_UART)
            uart_irq_service();
        if (pending & IRQ_SPI)
            spi_irq_service();
        if (pending & IRQ_I2C)
            i2c_irq_service();
        if (pending & IRQ_GPIO)
            gpio_irq_service();
        if (pending & IRQ_SNN)
            snn_irq_service();
    } else {
        /*
         * Synchronous exceptions (mcause bit 31=0) fall through and return
         * via mret without advancing mepc -- the faulting instruction will
         * be re-executed and re-fault, so this handler will be re-entered
         * on every subsequent cycle and mainline code never runs again.
         * dbg_trap_dump_pending() is therefore called directly here (unlike
         * the external-interrupt path above, which defers to mainline) --
         * a genuine exception is a one-time, catastrophic dead end, not a
         * recurring high-frequency source, so there is no storm/starvation
         * risk from printing here, and it is the ONLY chance this ever
         * prints (mainline's own dbg_trap_dump_pending() call, e.g. in
         * main.c's loop, will never be reached again). Firmware images
         * that don't call dbg_trap_dump_pending() themselves (most
         * firmware/test/test_*.c images) still get this for free.
         */
        dbg_trap_dump_pending();
    }
    /*
     * For a verification target, falling through to mret on a synchronous
     * exception is acceptable; production code should halt or record the
     * faulting mepc/mtval here.
     */
}
