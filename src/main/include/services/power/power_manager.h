#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*power_manager_transition_callback_t)(
    void *user_data);

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
    esp_err_t (*release_face_boost)(const char *reason);
    void (*mark_backlight_off)(void);

    void (*setup_error)(
        power_manager_setup_stage_t stage,
        esp_err_t error);
} power_manager_hooks_t;

esp_err_t power_manager_setup(
    const power_manager_hooks_t *hooks);

esp_err_t power_manager_start_inactivity_policy(void);

#ifdef __cplusplus
}
#endif
