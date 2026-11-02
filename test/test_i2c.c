/*
 * test_i2c.c — I2C master driver / RTL test
 * Links as a standalone ELF with its own main().
 * No stdlib, no printf.
 *
 * Co-designed with SoC/RTL_FIRMWARE/tests/test_i2c.py: the cocotb bench
 * acts as the I2C slave on the sda_test_in hook (see i2c_master.sv's
 * COCOTB_SIM ports). Transfer 1: bench ACKs read address 0xA1 and feeds back
 * data byte 0x5A. Transfer 2: bench NACKs address 0x55, so the master
 * must abort with STATUS.NACK_ERR set. Run standalone (sda_test_in tied
 * low = permanent ACK) transfer 2's nack check fails and this image
 * prints FAIL — that is expected outside the bench.
 *
 * Exercises the verified RTL contract (i2c_top.sv, i2c_master.sv,
 * i2c_regs.sv): CLKDIV write, CONTROL (IRQ-enable) write, TX write
 * triggering the fixed START/addr/ACK/data/NACK/STOP sequence, BUSY
 * polling, RX readback, NACK-error status, done-IRQ pulse.
 */

#include "uart.h"
#include "i2c.h"

int main(void)
{
    int nack1, nack2;
    uint8_t rx;

    uart_init(115200U);
    uart_puts("i2c test start\r\n");

    i2c_init(9U);                      /* CLKDIV=9: SCL = 50 MHz / 20 */
    I2C_REG(I2C_REG_CONTROL) = 1U;     /* done-IRQ pulse, observed by bench */

    /* Transfer 1: bench ACKs, sends back 0x5A */
    nack1 = 0;
    rx = i2c_transfer(0xA1U, &nack1);
    uart_puts("i2c rx: ");
    uart_put_hex32((uint32_t)rx);
    uart_puts("\r\n");
    uart_puts(nack1 ? "i2c nack1: 1\r\n" : "i2c nack1: 0\r\n");

    /* Transfer 2: bench NACKs the address */
    nack2 = 0;
    (void)i2c_transfer(0x55U, &nack2);
    uart_puts(nack2 ? "i2c nack2: 1\r\n" : "i2c nack2: 0\r\n");

    if (rx == 0x5AU && nack1 == 0 && nack2 == 1) {
        uart_puts("i2c PASS\r\n");
    } else {
        uart_puts("i2c FAIL\r\n");
    }
    while (1) { __asm__ volatile ("wfi"); }
    return 0;
}
