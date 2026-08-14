#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef esp_err_t (*cpu_power_idle_transition_callback_t)(void *user_data);

/* Initialize the verified 360 MHz ACTIVE policy. */
esp_err_t cpu_power_init(void);

/*
 * Register application transitions used around the staged IDLE-SCAN policy.
 * enter_idle_scan must suspend MIPI-DSI before the clock is reduced.
 * exit_idle_scan is invoked only after 360 MHz has been restored.
 */
esp_err_t cpu_power_register_idle_scan_callbacks(
    cpu_power_idle_transition_callback_t enter_idle_scan,
    cpu_power_idle_transition_callback_t exit_idle_scan,
    void *user_data);

/* Protect one detector/recognizer operation against a clock transition. */
esp_err_t cpu_power_ai_begin(void);
esp_err_t cpu_power_ai_end(void);

/* Select ACTIVE, IDLE-SCAN-180 or IDLE-SCAN-90 from inactivity duration. */
void cpu_power_update_inactivity(uint32_t inactive_ms);

/* Restore 360 MHz and request display restoration after accepted activity. */
void cpu_power_notify_activity(void);

/* Return 2, 8 or 16 frames for ACTIVE, 180 MHz or 90 MHz scanning. */
uint32_t cpu_power_get_face_detect_interval_frames(void);

bool cpu_power_is_idle_scan_active(void);

#ifdef __cplusplus
}
#endif
