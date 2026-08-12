#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Configure dynamic CPU-frequency scaling:
 * minimum = 40 MHz
 * maximum = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ (360 MHz in the current sdkconfig)
 * automatic Light-sleep disabled
 */
esp_err_t cpu_power_init(void);

/**
 * Force the CPU to the configured maximum frequency.
 */
esp_err_t cpu_power_ai_begin(void);

/**
 * Release the maximum-frequency requirement.
 */
esp_err_t cpu_power_ai_end(void);

#ifdef __cplusplus
}
#endif
