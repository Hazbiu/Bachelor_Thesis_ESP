
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef esp_err_t (*cpu_power_idle_transition_callback_t)(void *user_data);

/* Initialize the saved policy: existing 180..360 MHz DFS, or fixed max. */
esp_err_t cpu_power_init(void);

/* Before init: select the boot policy (startup task only).
 * After init: verify/apply the policy under the CPU policy mutex.
 * Disabling selects fixed APP_CPU_MAX_FREQ_MHZ, no software frame pacing,
 * and 100% active backlight. Explicit sleep modes remain independent. */
esp_err_t cpu_power_set_active_optimization_enabled(bool enabled);
bool cpu_power_active_optimization_is_enabled(void);

/* Call only after camera/display/AI suspension. Restore on every exit before
 * resuming hardware or entering Deep-sleep. Neither call changes saved settings.
 * A failed begin must still be paired with end to restore a partial change. */
esp_err_t cpu_power_begin_light_sleep_polling(void);
esp_err_t cpu_power_end_light_sleep_polling(void);

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

/* Return the existing optimized stride, or 1 in full-power mode. */
uint32_t cpu_power_get_face_detect_interval_frames(void);
/* Additional work-rate/brightness budgets for the current no-face stage.
 * Full-power mode returns 0 ms / 0 ms / 100%. Capture timing and models
 * remain configured by their existing camera/display/backend drivers. */
uint32_t cpu_power_get_ai_min_interval_ms(void);
uint32_t cpu_power_get_preview_min_interval_ms(void);
int cpu_power_get_backlight_percent(void);

/*
 * Runtime clocks used by the camera diagnostics overlay.
 * Both HP cores share SOC_MOD_CLK_CPU on ESP32-P4.
 */
uint32_t cpu_power_get_hp_frequency_hz(void);

/* Physical display-off idle scan is disabled while the camera is running. */
bool cpu_power_is_idle_scan_active(void);

#ifdef __cplusplus
}
#endif
