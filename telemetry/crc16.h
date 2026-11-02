#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>

/*
 * CRC16-Modbus
 * Polynomial : 0xA001 (reflected 0x8005)
 * Initial value: 0xFFFF
 * Input/output reflected: yes
 */

/** crc16_init() - Return initial CRC value 0xFFFF. */
uint16_t crc16_init(void);

/** crc16_update() - Feed len bytes into running CRC. */
uint16_t crc16_update(uint16_t crc, const uint8_t *data, uint32_t len);

/** crc16_compute() - One-shot: init + update over data[0..len-1]. */
uint16_t crc16_compute(const uint8_t *data, uint32_t len);

#endif /* CRC16_H */
