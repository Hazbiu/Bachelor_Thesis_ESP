#include "services/power/power_manager.h"

#include <stdbool.h>

#include "config/app_config.h"
#include "esp_err.h"
#include "esp_log.h"
#include "platform/power/cpu_power.h"
#include "services/power/sleep/app_sleep.h"
#include "services/power/component_runtime_policy.h"


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
        hooks->drain_active_work != NULL &&
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

    if (!s_hooks.drain_active_work(
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



esp_err_t power_manager_set_ethernet_enabled(bool enabled)
{
    return component_runtime_set_ethernet_enabled(enabled);
}

esp_err_t power_manager_set_wifi_enabled(bool enabled)
{
    return component_runtime_set_wifi_enabled(enabled);
}

esp_err_t power_manager_set_audio_enabled(bool enabled)
{
    return component_runtime_set_audio_enabled(enabled);
}

esp_err_t power_manager_set_sdcard_enabled(bool enabled)
{
    return component_runtime_set_sdcard_enabled(enabled);
}

esp_err_t power_manager_set_active_optimization_enabled(bool enabled)
{
    return cpu_power_set_active_optimization_enabled(enabled);
}

bool power_manager_active_optimization_is_enabled(void)
{
    return cpu_power_active_optimization_is_enabled();
}

bool power_manager_audio_policy_ready(void)
{
    return component_runtime_audio_policy_ready();
}

void power_manager_set_sleep_modes(bool light_enabled, bool deep_enabled)
{
    app_sleep_set_mode_policy(light_enabled, deep_enabled);
}

void power_manager_set_sleep_policy(
    bool light_enabled, bool deep_enabled,
    uint32_t light_delay_seconds, uint32_t deep_delay_seconds)
{
    app_sleep_set_policy(light_enabled, deep_enabled,
                         light_delay_seconds, deep_delay_seconds);
}

bool power_manager_pause_inactivity_policy(void)
{
    return app_sleep_pause_inactivity_policy();
}

void power_manager_resume_inactivity_policy(void)
{
    app_sleep_resume_inactivity_policy();
}

bool power_manager_inactivity_policy_is_paused(void)
{
    return app_sleep_inactivity_policy_is_paused();
}

void power_manager_notify_activity(void)
{
    app_sleep_notify_face_detected();
}

bool power_manager_sleep_is_requested(void)
{
    return app_sleep_is_requested();
}

bool power_manager_light_sleep_is_due(void)
{
    return app_sleep_light_sleep_is_due();
}

bool power_manager_deep_mode_is_enabled(void)
{
    return app_sleep_deep_mode_is_enabled();
}

void power_manager_request_deep_sleep(const char *reason)
{
    app_sleep_request(reason);
}

esp_err_t power_manager_apply_configuration(
    const power_manager_configuration_t *configuration)
{
    if (configuration == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    power_manager_set_sleep_policy(
        configuration->light_sleep_enabled,
        configuration->deep_sleep_enabled,
        configuration->light_sleep_delay_seconds,
        configuration->deep_sleep_delay_seconds);

    esp_err_t first_error = ESP_OK;

    /* The first call precedes CPU initialization and selects its boot policy.
     * Later applications are idempotent, including after Light-sleep resume.
     * Brightness on resume remains owned by the existing first-frame path. */
    esp_err_t ret = power_manager_set_active_optimization_enabled(
        configuration->active_optimization_enabled);
    if (ret != ESP_OK) {
        first_error = ret;
        ESP_LOGE(TAG, "Could not apply Active Mode Optimization: %s",
                 esp_err_to_name(ret));
    }

    ret = power_manager_set_wifi_enabled(configuration->wifi_enabled);
    if (ret != ESP_OK) {
        if (first_error == ESP_OK) {
            first_error = ret;
        }
        ESP_LOGE(TAG, "Could not apply saved Wi-Fi policy: %s", esp_err_to_name(ret));
    }

    ret = power_manager_set_ethernet_enabled(configuration->ethernet_enabled);
    if (ret != ESP_OK && first_error == ESP_OK) {
        first_error = ret;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not apply saved Ethernet policy: %s", esp_err_to_name(ret));
    }

    ret = power_manager_set_sdcard_enabled(configuration->sdcard_enabled);
    if (ret != ESP_OK && first_error == ESP_OK) {
        first_error = ret;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not apply saved microSD policy: %s", esp_err_to_name(ret));
    }

    if (power_manager_audio_policy_ready()) {
        ret = power_manager_set_audio_enabled(configuration->audio_enabled);
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Could not apply saved Audio policy: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGI(TAG, "Saved Audio policy will be applied after display/I2C startup");
    }

    return first_error;
}
