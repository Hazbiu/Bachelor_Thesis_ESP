#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Application Logic facade for the Configuration Service.
 *
 * UI -> Application Logic -> Configuration Service
 *                        -> Power Management (when a saved setting has a
 *                           hardware/power consequence)
 */

#define APP_CONFIGURATION_MAX_AUTHORIZED_USERS 24
#define APP_CONFIGURATION_MAX_USER_NAME_LENGTH 64

typedef struct {
    bool dark_mode;
    bool ethernet_enabled;
    bool wifi_enabled;
    bool camera_enabled;
    bool audio_enabled;
    bool sdcard_enabled;
    bool light_sleep_enabled;
    bool deep_sleep_enabled;
} app_configuration_snapshot_t;

typedef struct {
    size_t count;
    char names[APP_CONFIGURATION_MAX_AUTHORIZED_USERS]
              [APP_CONFIGURATION_MAX_USER_NAME_LENGTH];
} app_configuration_authorized_users_t;

esp_err_t app_configuration_init(void);
app_configuration_snapshot_t app_configuration_get(void);
esp_err_t app_configuration_apply_saved_policy(void);

esp_err_t app_configuration_set_dark_mode(bool enabled);
esp_err_t app_configuration_set_ethernet_enabled(bool enabled);
esp_err_t app_configuration_set_wifi_enabled(bool enabled);
esp_err_t app_configuration_set_camera_enabled(bool enabled);
esp_err_t app_configuration_set_audio_enabled(bool enabled);
esp_err_t app_configuration_set_sdcard_enabled(bool enabled);
esp_err_t app_configuration_set_light_sleep_enabled(bool enabled);
esp_err_t app_configuration_set_deep_sleep_enabled(bool enabled);

esp_err_t app_configuration_load_authorized_users(
    app_configuration_authorized_users_t *users_out);

#ifdef __cplusplus
}
#endif
