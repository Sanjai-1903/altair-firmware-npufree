#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>

/* ── Packet type codes ───────────────────────────────────────── */
#define PKT_HEARTBEAT       0x00U   /* payload: 4-byte uptime seconds    */
#define PKT_INFERENCE       0x01U   /* payload: 1 byte, 0=NORMAL 1=ANOMALY */
#define PKT_ANOMALY         0x02U   /* payload: 1-byte class index       */
#define PKT_LATENCY         0x03U   /* payload: 16-byte latency stats    */
#define PKT_ERROR           0xE0U   /* payload: 1-byte error code        */
#define PKT_DEBUG           0xFFU   /* payload: N-byte ASCII string      */

/* ── Error codes (PKT_ERROR payload) ──────────────────────────
 * NOTE: main.c sends g_inference_error_stage (a small pipeline-stage
 * number, see inference.h) as the PKT_ERROR payload, not one of the
 * 0xEx codes below. The 0xEx codes are the pre-existing reserved space
 * for firmware-level faults; keep the two ranges disjoint.
 *
 * ERR_NPU_TIMEOUT (0xE1) is RETIRED with the NPU block and is
 * deliberately left defined-as-reserved rather than reused, so a decoder
 * that still knows the old code cannot silently misread a new one.
 */
#define ERR_NPU_TIMEOUT_RETIRED 0xE1U  /* do not reuse */
#define ERR_SNN_TIMEOUT     0xE2U
#define ERR_DMA_TIMEOUT     0xE3U
#define ERR_STACK_OVERFLOW  0xE4U

/* ── API ─────────────────────────────────────────────────────── */

/** telemetry_init() - No hardware init needed (uses uart driver).
 *  Call after uart_init(). Reserved for future use. */
void telemetry_init(void);

/** telemetry_send() - Frame and transmit a packet.
 *  @type:    packet type (PKT_* constant)
 *  @payload: pointer to payload bytes (may be NULL if len=0)
 *  @len:     payload length in bytes
 */
void telemetry_send(uint8_t type, const uint8_t *payload, uint8_t len);

/** telemetry_send_heartbeat() - Send uptime counter as PKT_HEARTBEAT. */
void telemetry_send_heartbeat(uint32_t uptime_sec);

/** telemetry_send_inference() - Send classification (0=NORMAL, 1=ANOMALY)
 *  as PKT_INFERENCE. */
void telemetry_send_inference(int8_t score);

/** telemetry_send_anomaly() - Send class index as PKT_ANOMALY. */
void telemetry_send_anomaly(uint8_t class_idx);

/** telemetry_send_error() - Send error code as PKT_ERROR. */
void telemetry_send_error(uint8_t err_code);

/**
 * telemetry_send_latency() - Send PKT_LATENCY with four uint32_t fields.
 *
 * Packet format (PKT_LATENCY = 0x03, payload 16 bytes):
 *   Bytes  0– 3  last_us  — most recent inference latency  (µs, uint32 LE)
 *   Bytes  4– 7  avg_us   — running average latency        (µs, uint32 LE)
 *   Bytes  8–11  min_us   — minimum latency observed       (µs, uint32 LE)
 *   Bytes 12–15  max_us   — maximum latency observed       (µs, uint32 LE)
 *
 * All existing packet types are unchanged.
 * Receivers that do not recognise type 0x03 should skip it using LEN.
 */
void telemetry_send_latency(uint32_t last_us, uint32_t avg_us,
                            uint32_t min_us,  uint32_t max_us);

#endif /* TELEMETRY_H */
