/*
 * perf_metrics.h
 *
 *  Created on: Oct 31, 2026
 *      Author: hegde
 */

#ifndef INC_PERF_METRICS_H_
#define INC_PERF_METRICS_H_

#include "stm32f3xx_hal.h"
#include <stdint.h>

/* Structure to store benchmark results */
typedef struct {
    uint32_t erase_cycles;
    uint32_t total_decrypt_cycles;
    uint32_t total_flash_write_cycles;
    uint32_t sha256_cycles;
    uint32_t ecdsa_verify_cycles;
    uint32_t total_session_cycles;
    uint32_t chunk_count;
} EndECU_PerfMetrics_t;

extern EndECU_PerfMetrics_t g_perf_metrics;

/* Enable DWT Cycle Counter */
static inline void DWT_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;  // Enable Trace
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;             // Enable CYCCNT
}

/* Get current cycle count */
static inline uint32_t DWT_GetCycles(void)
{
    return DWT->CYCCNT;
}

/* Reset counter */
static inline void DWT_ResetCycles(void)
{
    DWT->CYCCNT = 0;
}

/* Helper to convert CPU cycles to milliseconds */
static inline float Cycles_To_Ms(uint32_t cycles, uint32_t cpu_freq_hz)
{
    return ((float)cycles * 1000.0f) / (float)cpu_freq_hz;
}

#endif /* INC_PERF_METRICS_H_ */
