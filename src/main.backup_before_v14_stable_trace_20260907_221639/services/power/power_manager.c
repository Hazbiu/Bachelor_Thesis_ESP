#include "services/power/power_manager.h"

#include <stdbool.h>

#include "config/app_config.h"
#include "esp_err.h"
#include "esp_log.h"
#include "platform/power/cpu_power.h"
#include "services/power/sleep/app_sleep.h"
#include "services/vision/ai_worker_state.h"


static const char *TAG = "app_main";
static power_manager_hooks_t s_hooks;


static bool hooks_are_complete(
    const power_manager_hooks_t *hooks)
{
    return hooks != NULL &&
        hooks->light_sleep_suspend != NULL &&
        hooks->light_sleep_resume != NULL &&
        hooks->idle_scan_suspend != NULL &&
        hooks->idle_scan_resume != NULL &&
        hooks->deep_sleep_state_requested != NULL &&
        hooks->block_new_work != NULL &&
        hooks->release_face_boost != NULL &&
        hooks->mark_backlight_off != NULL;
}


static void report_setup_error(
    power_manager_setup_stage_t stage,
    esp_err_t error)
{
    if (error != ESP_OK && s_hooks.setup_error != NULL) {
        s_hooks.setup_error(stage, error);
    }
}


static void prepare_application_for_deep_sleep(
    void *user_data)
{
    (void)user_data;

    s_hooks.deep_sleep_state_requested();
    s_hooks.block_new_work();

    if (!vision_ai_worker_state_pause_and_drain(
            APP_AI_WORKER_DRAIN_TIMEOUT_MS)) {
        ESP_LOGW(
            TAG,
            "AI worker did not drain before Deep-sleep shutdown");
    }

    (void)s_hooks.release_face_boost(
        "application sleep requested");

    s_hooks.mark_backlight_off();
}


esp_err_t power_manager_setup(
    const power_manager_hooks_t *hooks)
{
    if (!hooks_are_complete(hooks)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_hooks = *hooks;

    esp_err_t first_error = ESP_OK;

    esp_err_t ret =
        app_sleep_register_light_sleep_callbacks(
            s_hooks.light_sleep_suspend,
            s_hooks.light_sleep_resume,
            NULL);

    if (ret != ESP_OK) {
        first_error = ret;
        report_setup_error(
            POWER_MANAGER_SETUP_LIGHT_SLEEP_CALLBACKS,
            ret);
    }

    ret = cpu_power_register_idle_scan_callbacks(
        s_hooks.idle_scan_suspend,
        s_hooks.idle_scan_resume,
        NULL);

    if (ret != ESP_OK) {
        if (first_error == ESP_OK) {
            first_error = ret;
        }

        report_setup_error(
            POWER_MANAGER_SETUP_IDLE_SCAN_CALLBACKS,
            ret);
    }

    ret = app_sleep_start_button_monitor(
        prepare_application_for_deep_sleep,
        NULL);

    if (ret != ESP_OK) {
        if (first_error == ESP_OK) {
            first_error = ret;
        }

        report_setup_error(
            POWER_MANAGER_SETUP_BUTTON_MONITOR,
            ret);
    }

    return first_error;
}


esp_err_t power_manager_start_inactivity_policy(void)
{
    return app_sleep_start_timeout();
}
