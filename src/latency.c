/*
 * latency.c — inference latency statistics
 * No stdlib / POSIX headers. No float. No 64-bit arithmetic.
 *
 * Uses incremental (Welford/Knuth) average: only 32-bit divu required.
 * RV32IM M-extension divu handles uint32_t / uint32_t natively — no libgcc.
 * g_count saturates at UINT32_MAX (~4.3 billion samples).
 */

#include "latency.h"

static uint32_t g_last_us;
static uint32_t g_avg_us;
static uint32_t g_min_us = 0xFFFFFFFFUL;   /* sentinel: not yet set */
static uint32_t g_max_us;
static uint32_t g_count;

void latency_update(uint32_t us)
{
    g_last_us = us;
    if (us < g_min_us) g_min_us = us;
    if (us > g_max_us) g_max_us = us;

    /* Incremental average: avg += (sample - avg) / count
     * Uses only 32-bit unsigned arithmetic — no __udivdi3.            */
    if (g_count == 0U) {
        g_avg_us = us;
    } else {
        uint32_t n = g_count + 1U;               /* sample count after this one */
        if (us >= g_avg_us) {
            g_avg_us += (us - g_avg_us) / n;
        } else {
            g_avg_us -= (g_avg_us - us) / n;
        }
    }

    if (g_count < 0xFFFFFFFFUL) g_count++;
}

int latency_get_stats(uint32_t *last_us, uint32_t *avg_us,
                      uint32_t *min_us,  uint32_t *max_us)
{
    if (g_count == 0U) {
        *last_us = 0U;
        *avg_us  = 0U;
        *min_us  = 0U;
        *max_us  = 0U;
        return 0;
    }
    *last_us = g_last_us;
    *avg_us  = g_avg_us;
    *min_us  = g_min_us;
    *max_us  = g_max_us;
    return 1;
}

uint32_t latency_get_count(void)
{
    return g_count;
}

void latency_reset(void)
{
    g_last_us = 0U;
    g_avg_us  = 0U;
    g_min_us  = 0xFFFFFFFFUL;
    g_max_us  = 0U;
    g_count   = 0U;
}
