#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize one verified, display-safe active DFS range:
 *
 * All active profiles: 90..CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ (360 MHz here)
 * PERFORMANCE: application AI maximum-frequency lock enabled
 * BALANCED/ECO: application AI maximum-frequency lock disabled
 *
 * Automatic Light-sleep remains disabled because app_sleep.c explicitly
 * coordinates the camera, display, LVGL and GT911 before sleeping.
 */
esp_err_t cpu_power_init(void);

/**
 * Request the configured 360 MHz maximum during AI inference only when the
 * PERFORMANCE profile is active. This is a successful no-op in BALANCED/ECO,
 * allowing DFS to select 90 MHz between work while preserving driver scaling.
 */
esp_err_t cpu_power_ai_begin(void);

/** Release the PERFORMANCE AI lock acquired by cpu_power_ai_begin(). */
esp_err_t cpu_power_ai_end(void);

/**
 * Dynamically select PERFORMANCE, BALANCED or ECO from inactivity duration.
 * BALANCED/ECO keep the safe 90..360 MHz DFS range, release the application's
 * AI maximum-frequency request and progressively throttle face detection.
 */
void cpu_power_update_inactivity(uint32_t inactive_ms);

/** Restore PERFORMANCE AI locking immediately after accepted activity. */
void cpu_power_notify_activity(void);

/** Return the face-detection interval for the current active power profile. */
uint32_t cpu_power_get_face_detect_interval_frames(void);

#ifdef __cplusplus
}
#endif
