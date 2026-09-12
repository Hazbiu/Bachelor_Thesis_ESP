#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*power_manager_transition_callback_t)(void *user_data);

typedef enum {
    POWER_MANAGER_SETUP_LIGHT_SLEEP_CALLBACKS = 0,
    POWER_MANAGER_SETUP_IDLE_SCAN_CALLBACKS,
    POWER_MANAGER_SETUP_BUTTON_MONITOR,
} power_manager_setup_stage_t;

typedef struct {
    power_manager_transition_callback_t light_sleep_suspend;
    power_manager_transition_callback_t light_sleep_resume;
    power_manager_transition_callback_t idle_scan_suspend;
    power_manager_transition_callback_t idle_scan_resume;

    void (*deep_sleep_state_requested)(void);
    void (*block_new_work)(void);
    bool (*drain_active_work)(uint32_t timeout_ms);
    esp_err_t (*release_face_boost)(const char *reason);
    void (*mark_backlight_off)(void);

    void (*setup_error)(
        power_manager_setup_stage_t stage,
        esp_err_t error);
} power_manager_hooks_t;

typedef struct {
    bool ethernet_enabled;
    bool wifi_enabled;
    bool audio_enabled;
    bool sdcard_enabled;
    bool active_optimization_enabled;
    bool light_sleep_enabled;
    bool deep_sleep_enabled;
} power_manager_configuration_t;

esp_err_t power_manager_setup(const power_manager_hooks_t *hooks);
esp_err_t power_manager_start_inactivity_policy(void);

/* Application-facing Power Management API. */
esp_err_t power_manager_apply_configuration(
    const power_manager_configuration_t *configuration);
esp_err_t power_manager_set_ethernet_enabled(bool enabled);
esp_err_t power_manager_set_wifi_enabled(bool enabled);
esp_err_t power_manager_set_audio_enabled(bool enabled);
esp_err_t power_manager_set_sdcard_enabled(bool enabled);
esp_err_t power_manager_set_active_optimization_enabled(bool enabled);
bool power_manager_active_optimization_is_enabled(void);
bool power_manager_audio_policy_ready(void);
void power_manager_set_sleep_modes(bool light_enabled, bool deep_enabled);
bool power_manager_pause_inactivity_policy(void);
void power_manager_resume_inactivity_policy(void);
bool power_manager_inactivity_policy_is_paused(void);
void power_manager_notify_activity(void);
bool power_manager_sleep_is_requested(void);
bool power_manager_light_sleep_is_due(void);
bool power_manager_deep_mode_is_enabled(void);
void power_manager_request_deep_sleep(const char *reason);

#ifdef __cplusplus
}
#endif
