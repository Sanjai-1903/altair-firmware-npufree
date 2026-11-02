/*
 * telemetry.c — Packet framing over bare-metal UART
 * No stdlib / POSIX headers. No memcpy. No float.
 *
 * Frame format:
 *   [0x7E][LEN][TYPE][PAYLOAD 0..N][CRC_L][CRC_H][0x7F]
 *
 *   0x7E     = frame start
 *   LEN      = payload byte count (1 byte, max 255)
 *   TYPE     = packet type byte
 *   PAYLOAD  = LEN bytes of data
 *   CRC_L/H  = CRC16-Modbus over [TYPE + PAYLOAD], little-endian
 *   0x7F     = frame end
 */

#include "telemetry.h"
#include "crc16.h"
#include "uart.h"

/* ── Public API ──────────────────────────────────────────────────────────── */

void telemetry_init(void)
{
    /* No hardware init required — UART driver must be initialised by caller.
     * Reserved for future state setup (e.g. sequence numbers, filters). */
}

void telemetry_send(uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint16_t crc;
    uint8_t  i;
    uint8_t  type_buf[1];

    /* ── Frame start ── */
    uart_putc(0x7EU);

    /* ── Header ── */
    uart_putc(len);
    uart_putc(type);

    /* ── Payload bytes ── */
    for (i = 0U; i < len; i++) {
        uart_putc(payload[i]);
    }

    /* ── CRC16 over TYPE then PAYLOAD ── */
    type_buf[0] = type;
    crc = crc16_compute(type_buf, 1U);
    if (len > 0U) {
        crc = crc16_update(crc, payload, (uint32_t)len);
    }

    uart_putc((uint8_t)(crc & 0xFFU));          /* CRC low byte  */
    uart_putc((uint8_t)((crc >> 8) & 0xFFU));   /* CRC high byte */

    /* ── Frame end ── */
    uart_putc(0x7FU);
}

void telemetry_send_heartbeat(uint32_t uptime_sec)
{
    uint8_t buf[4];

    /* Pack uptime_sec as 4 bytes little-endian — no memcpy */
    buf[0] = (uint8_t)(uptime_sec        & 0xFFU);
    buf[1] = (uint8_t)((uptime_sec >> 8)  & 0xFFU);
    buf[2] = (uint8_t)((uptime_sec >> 16) & 0xFFU);
    buf[3] = (uint8_t)((uptime_sec >> 24) & 0xFFU);

    telemetry_send(PKT_HEARTBEAT, buf, 4U);
}

void telemetry_send_inference(int8_t score)
{
    uint8_t b = (uint8_t)score;
    telemetry_send(PKT_INFERENCE, &b, 1U);
}

void telemetry_send_anomaly(uint8_t class_idx)
{
    telemetry_send(PKT_ANOMALY, &class_idx, 1U);
}

void telemetry_send_error(uint8_t err_code)
{
    telemetry_send(PKT_ERROR, &err_code, 1U);
}

void telemetry_send_latency(uint32_t last_us, uint32_t avg_us,
                            uint32_t min_us,  uint32_t max_us)
{
    uint8_t buf[16];

    /* Pack four uint32_t values as little-endian — no memcpy */
    buf[ 0] = (uint8_t)( last_us        & 0xFFU);
    buf[ 1] = (uint8_t)((last_us >>  8) & 0xFFU);
    buf[ 2] = (uint8_t)((last_us >> 16) & 0xFFU);
    buf[ 3] = (uint8_t)((last_us >> 24) & 0xFFU);
    buf[ 4] = (uint8_t)( avg_us         & 0xFFU);
    buf[ 5] = (uint8_t)((avg_us  >>  8) & 0xFFU);
    buf[ 6] = (uint8_t)((avg_us  >> 16) & 0xFFU);
    buf[ 7] = (uint8_t)((avg_us  >> 24) & 0xFFU);
    buf[ 8] = (uint8_t)( min_us         & 0xFFU);
    buf[ 9] = (uint8_t)((min_us  >>  8) & 0xFFU);
    buf[10] = (uint8_t)((min_us  >> 16) & 0xFFU);
    buf[11] = (uint8_t)((min_us  >> 24) & 0xFFU);
    buf[12] = (uint8_t)( max_us         & 0xFFU);
    buf[13] = (uint8_t)((max_us  >>  8) & 0xFFU);
    buf[14] = (uint8_t)((max_us  >> 16) & 0xFFU);
    buf[15] = (uint8_t)((max_us  >> 24) & 0xFFU);

    telemetry_send(PKT_LATENCY, buf, 16U);
}
