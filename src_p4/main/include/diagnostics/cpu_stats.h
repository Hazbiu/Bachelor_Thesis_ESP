#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional verbose debug table retained for manual diagnostics.
 * This remains controlled by APP_DIAGNOSTICS_CPU_STATS_ENABLED.
 */
void diagnostics_start_cpu_stats_monitor(void);

/*
 * Lightweight live utilization telemetry for the two ESP32-P4 HP cores.
 *
 * One very-low-priority sampler is pinned to HP Core 0 and one to HP Core 1.
 * Each sampler reads the FreeRTOS IDLE run-time counter of its own core once
 * per second.  The camera overlay reads only the cached percentages.
 */
esp_err_t diagnostics_cpu_hp_usage_start(void);
void diagnostics_cpu_hp_usage_reset(void);

/*
 * Return the latest complete utilization samples in tenths of a percent:
 *
 *     0    =   0.0 %
 *     347  =  34.7 %
 *     1000 = 100.0 %
 *
 * Returns false until both HP cores have produced a complete one-second
 * sample after startup/reset.
 */
bool diagnostics_cpu_hp_usage_snapshot(
    uint32_t *core0_usage_x10,
    uint32_t *core1_usage_x10);

#ifdef __cplusplus
}
#endif
