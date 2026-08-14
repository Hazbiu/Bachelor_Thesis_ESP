#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the experimental fixed-frequency CPU sweep.
 *
 * The initial CPU clock is fixed at CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ.
 * Automatic Light-sleep remains disabled because app_sleep.c coordinates the
 * camera, display, LVGL and touchscreen before manually entering sleep.
 */
esp_err_t cpu_power_init(void);

/**
 * Protect an AI inference operation from a frequency transition.
 *
 * The current stage's ESP_PM_CPU_FREQ_MAX lock is acquired and the policy
 * mutex remains held until cpu_power_ai_end() is called.
 */
esp_err_t cpu_power_ai_begin(void);

/**
 * Release the current-stage CPU-frequency lock and policy mutex.
 */
esp_err_t cpu_power_ai_end(void);

/**
 * Select a fixed CPU-frequency stage according to the inactivity duration.
 *
 * The configured sweep is:
 * 360, 240, 180, 120, 90, 60 and 40 MHz.
 */
void cpu_power_update_inactivity(uint32_t inactive_ms);

/**
 * Immediately restore the full-performance frequency after accepted activity.
 */
void cpu_power_notify_activity(void);

/**
 * Return the fixed face-detection interval used during the experiment.
 */
uint32_t cpu_power_get_face_detect_interval_frames(void);

#ifdef __cplusplus
}
#endif
