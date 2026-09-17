#include "app/configuration/app_configuration.h"

#include <string.h>

#include "esp_log.h"
#include "domain/ports/system_adapters_port.h"
#include "services/power/power_manager.h"
#include "services/settings/app_settings.h"

static const char *TAG = "app_configuration";

_Static_assert(
    APP_CONFIGURATION_MAX_AUTHORIZED_USERS == APP_SETTINGS_MAX_AUTHORIZED_USERS,
    "Application/configuration authorized-user capacity must match the service");
_Static_assert(
    APP_CONFIGURATION_MAX_USER_NAME_LENGTH == APP_SETTINGS_MAX_USER_NAME_LENGTH,
    "Application/configuration user-name size must match the service");

static app_configuration_snapshot_t from_service(
    const app_settings_snapshot_t settings)
{
    const app_configuration_snapshot_t configuration = {
        .dark_mode = settings.dark_mode,
        .ethernet_enabled = settings.ethernet_enabled,
        .wifi_enabled = settings.wifi_enabled,
        .camera_enabled = settings.camera_enabled,
        .audio_enabled = settings.audio_enabled,
        .sdcard_enabled = settings.sdcard_enabled,
        .active_optimization_enabled = settings.active_optimization_enabled,
        .light_sleep_enabled = settings.light_sleep_enabled,
        .deep_sleep_enabled = settings.deep_sleep_enabled,
        .light_sleep_delay_seconds = settings.light_sleep_delay_seconds,
        .deep_sleep_delay_seconds = settings.deep_sleep_delay_seconds,
    };
    return configuration;
}

static power_manager_configuration_t to_power_configuration(
    const app_settings_snapshot_t settings)
{
    const power_manager_configuration_t configuration = {
        .ethernet_enabled = settings.ethernet_enabled,
        .wifi_enabled = settings.wifi_enabled,
        .audio_enabled = settings.audio_enabled,
        .sdcard_enabled = settings.sdcard_enabled,
        .active_optimization_enabled = settings.active_optimization_enabled,
        .light_sleep_enabled = settings.light_sleep_enabled,
        .deep_sleep_enabled = settings.deep_sleep_enabled,
        .light_sleep_delay_seconds = settings.light_sleep_delay_seconds,
        .deep_sleep_delay_seconds = settings.deep_sleep_delay_seconds,
    };
    return configuration;
}

esp_err_t app_configuration_init(void)
{
    const esp_err_t ret = app_settings_init();

    /* Keep Power Management synchronized even when NVS is unavailable and
     * Configuration Service falls back to its in-memory defaults. */
    const app_settings_snapshot_t settings = app_settings_get();
    power_manager_set_sleep_policy(
        settings.light_sleep_enabled,
        settings.deep_sleep_enabled,
        settings.light_sleep_delay_seconds,
        settings.deep_sleep_delay_seconds);

    return ret;
}

app_configuration_snapshot_t app_configuration_get(void)
{
    return from_service(app_settings_get());
}

esp_err_t app_configuration_apply_saved_policy(void)
{
    const app_settings_snapshot_t settings = app_settings_get();
    const power_manager_configuration_t power = to_power_configuration(settings);
    const esp_err_t ret = power_manager_apply_configuration(&power);

    ESP_LOGI(
        TAG,
        "Camera policy applied: pipeline=%s",
        settings.camera_enabled ? "available" : "blocked and uninitialized");

    return ret;
}

esp_err_t app_configuration_set_dark_mode(bool enabled)
{
    return app_settings_set_dark_mode(enabled);
}

esp_err_t app_configuration_set_camera_enabled(bool enabled)
{
    return app_settings_set_camera_enabled(enabled);
}

esp_err_t app_configuration_set_ethernet_enabled(bool enabled)
{
    const app_settings_snapshot_t previous = app_settings_get();
    if (previous.ethernet_enabled == enabled) {
        return ESP_OK;
    }

    esp_err_t ret = power_manager_set_ethernet_enabled(enabled);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = app_settings_set_ethernet_enabled(enabled);
    if (ret != ESP_OK) {
        (void)power_manager_set_ethernet_enabled(previous.ethernet_enabled);
    }
    return ret;
}

esp_err_t app_configuration_set_wifi_enabled(bool enabled)
{
    const app_settings_snapshot_t previous = app_settings_get();
    if (previous.wifi_enabled == enabled) {
        return ESP_OK;
    }

    esp_err_t ret = power_manager_set_wifi_enabled(enabled);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = app_settings_set_wifi_enabled(enabled);
    if (ret != ESP_OK) {
        (void)power_manager_set_wifi_enabled(previous.wifi_enabled);
    }
    return ret;
}

esp_err_t app_configuration_set_audio_enabled(bool enabled)
{
    const app_settings_snapshot_t previous = app_settings_get();
    if (previous.audio_enabled == enabled) {
        return ESP_OK;
    }

    esp_err_t ret = power_manager_set_audio_enabled(enabled);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = app_settings_set_audio_enabled(enabled);
    if (ret != ESP_OK) {
        (void)power_manager_set_audio_enabled(previous.audio_enabled);
    }
    return ret;
}

esp_err_t app_configuration_set_sdcard_enabled(bool enabled)
{
    const app_settings_snapshot_t previous = app_settings_get();
    if (previous.sdcard_enabled == enabled) {
        return ESP_OK;
    }

    esp_err_t ret = power_manager_set_sdcard_enabled(enabled);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = app_settings_set_sdcard_enabled(enabled);
    if (ret != ESP_OK) {
        (void)power_manager_set_sdcard_enabled(previous.sdcard_enabled);
    }
    return ret;
}

esp_err_t app_configuration_set_active_optimization_enabled(bool enabled)
{
    const app_settings_snapshot_t previous = app_settings_get();
    if (previous.active_optimization_enabled == enabled) {
        return ESP_OK;
    }

    /* This UI action runs from Settings on the launcher, before camera startup.
     * Apply hardware first; the existing UI worker saves and confirms the switch
     * only after both the CPU policy and the physical brightness succeed. */
    esp_err_t ret = power_manager_set_active_optimization_enabled(enabled);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = system_display_backlight_set_percent(system_cpu_backlight_percent());
    if (ret == ESP_OK) {
        ret = app_settings_set_active_optimization_enabled(enabled);
    }

    if (ret != ESP_OK) {
        const esp_err_t cpu_restore = power_manager_set_active_optimization_enabled(
            previous.active_optimization_enabled);
        const esp_err_t display_restore = system_display_backlight_set_percent(
            system_cpu_backlight_percent());
        if (cpu_restore != ESP_OK || display_restore != ESP_OK) {
            ESP_LOGE(TAG, "Active optimization rollback failed: CPU=%s display=%s",
                     esp_err_to_name(cpu_restore), esp_err_to_name(display_restore));
        }
    }
    return ret;
}

static void synchronize_sleep_policy(void)
{
    const app_settings_snapshot_t settings = app_settings_get();
    power_manager_set_sleep_policy(
        settings.light_sleep_enabled,
        settings.deep_sleep_enabled,
        settings.light_sleep_delay_seconds,
        settings.deep_sleep_delay_seconds);
}

esp_err_t app_configuration_set_light_sleep_enabled(bool enabled)
{
    const esp_err_t ret = app_settings_set_light_sleep_enabled(enabled);
    if (ret == ESP_OK) {
        synchronize_sleep_policy();
    }
    return ret;
}

esp_err_t app_configuration_set_deep_sleep_enabled(bool enabled)
{
    const esp_err_t ret = app_settings_set_deep_sleep_enabled(enabled);
    if (ret == ESP_OK) {
        synchronize_sleep_policy();
    }
    return ret;
}

esp_err_t app_configuration_set_light_sleep_delay_seconds(uint32_t seconds)
{
    const esp_err_t ret = app_settings_set_light_sleep_delay_seconds(seconds);
    if (ret == ESP_OK) {
        synchronize_sleep_policy();
    }
    return ret;
}

esp_err_t app_configuration_set_deep_sleep_delay_seconds(uint32_t seconds)
{
    const esp_err_t ret = app_settings_set_deep_sleep_delay_seconds(seconds);
    if (ret == ESP_OK) {
        synchronize_sleep_policy();
    }
    return ret;
}

esp_err_t app_configuration_load_authorized_users(
    app_configuration_authorized_users_t *users_out)
{
    if (users_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const app_settings_snapshot_t settings = app_settings_get();
    const bool restore_saved_off_state = !settings.sdcard_enabled;

    esp_err_t ret = power_manager_set_sdcard_enabled(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not power/mount microSD for user refresh: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    app_settings_authorized_users_t service_users = {0};
    const esp_err_t scan_ret = app_settings_load_authorized_users(&service_users);

    esp_err_t restore_ret = ESP_OK;
    if (restore_saved_off_state) {
        restore_ret = power_manager_set_sdcard_enabled(false);
        if (restore_ret != ESP_OK) {
            ESP_LOGE(TAG, "Could not restore saved microSD OFF state: %s",
                     esp_err_to_name(restore_ret));
        }
    }

    memset(users_out, 0, sizeof(*users_out));
    if (scan_ret == ESP_OK) {
        users_out->count = service_users.count;
        memcpy(users_out->names, service_users.names, sizeof(users_out->names));
    }

    if (scan_ret != ESP_OK) {
        return scan_ret;
    }
    return restore_ret;
}
