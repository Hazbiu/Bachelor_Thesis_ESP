#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef esp_err_t (*cpu_power_idle_transition_callback_t)(void *user_data);

/* Initialize the verified 180..360 MHz ACTIVE DFS policy. */
esp_err_t cpu_power_init(void);

/*
 * Compatibility registration for the former display-off IDLE-SCAN policy.
 * The callbacks are intentionally not invoked while CSI/ISP is active because
 * deleting the shared display/DPHY resources can fault the camera ISP ISR.
 */
esp_err_t cpu_power_register_idle_scan_callbacks(
    cpu_power_idle_transition_callback_t enter_idle_scan,
    cpu_power_idle_transition_callback_t exit_idle_scan,
    void *user_data);

/*
 * Hold the ESP_PM_CPU_FREQ_MAX lock across the complete face-authentication
 * burst. Both calls are idempotent: duplicate begin/end calls are harmless.
 */
esp_err_t cpu_power_face_boost_begin(void);
esp_err_t cpu_power_face_boost_end(void);
bool cpu_power_is_face_boost_active(void);

/* Select ACTIVE, ECO-SCAN-8 or ECO-SCAN-16 detection duty cycle. */
void cpu_power_update_inactivity(uint32_t inactive_ms);

/* Restore the ACTIVE baseline policy and request display restoration. */
void cpu_power_notify_activity(void);

/* Return 2, 8 or 16 frames for ACTIVE, ECO-SCAN-8 or ECO-SCAN-16. */
uint32_t cpu_power_get_face_detect_interval_frames(void);

/* Physical display-off idle scan is disabled while the camera is running. */
bool cpu_power_is_idle_scan_active(void);

#ifdef __cplusplus
}
#endif
