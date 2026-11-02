#ifndef LATENCY_H
#define LATENCY_H

/*
 * latency.h — inference latency tracking
 * Accumulates last / average / min / max latency in microseconds.
 * No stdlib / POSIX headers. No float. No 64-bit arithmetic.
 *
 * Intended call sequence per inference (see inference.c, which brackets
 * the two Conv1D layers and both pooling stages -- the work that used to
 * run on the NPU):
 *   t0 = timer_get_cycles();
 *   ... the stage being measured ...
 *   t1 = timer_get_cycles();
 *   latency_update((t1 - t0) / (SYS_CLK_HZ / 1000000U));
 *
 * Average uses incremental (Welford/Knuth) update — only 32-bit divu.
 * Count saturates at UINT32_MAX.
 */

#include <stdint.h>

/**
 * latency_update() - Record one latency sample.
 * @us: latency of the most recent inference in microseconds.
 * Updates last, min, max, and the average accumulator.
 * Not interrupt-safe — call only from the inference path (main context).
 */
void latency_update(uint32_t us);

/**
 * latency_get_stats() - Snapshot all four statistics atomically.
 * @last_us, @avg_us, @min_us, @max_us: output pointers (must not be NULL).
 * All outputs are 0 if no samples have been recorded yet.
 * Returns 1 if at least one sample exists, 0 if count is zero.
 */
int latency_get_stats(uint32_t *last_us, uint32_t *avg_us,
                      uint32_t *min_us,  uint32_t *max_us);

/** latency_get_count() - Return the total number of samples recorded. */
uint32_t latency_get_count(void);

/**
 * latency_reset() - Clear all statistics and restart tracking.
 * Call to begin a fresh measurement window (e.g. after a firmware event).
 */
void latency_reset(void);

#endif /* LATENCY_H */
