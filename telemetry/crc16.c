/*
 * crc16.c — CRC16-Modbus checksum
 * Polynomial : 0xA001 (reflected 0x8005)
 * Initial value: 0xFFFF
 * No stdlib, no lookup table — bit-by-bit to conserve IRAM.
 */

#include "crc16.h"

uint16_t crc16_init(void)
{
    return 0xFFFFU;
}

uint16_t crc16_update(uint16_t crc, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    uint8_t  bit;

    for (i = 0U; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (bit = 0U; bit < 8U; bit++) {
            if (crc & 1U) {
                crc = (crc >> 1) ^ 0xA001U;
            } else {
                crc = crc >> 1;
            }
        }
    }
    return crc;
}

uint16_t crc16_compute(const uint8_t *data, uint32_t len)
{
    return crc16_update(crc16_init(), data, len);
}
