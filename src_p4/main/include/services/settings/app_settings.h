#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_SETTINGS_MAX_AUTHORIZED_USERS 24
#define APP_SETTINGS_MAX_USER_NAME_LENGTH 64

typedef struct {
    bool dark_mode;
    bool ethernet_enabled;
    bool wifi_enabled;
    bool camera_enabled;
    bool audio_enabled;
    bool sdcard_enabled;
    bool active_optimization_enabled;
    bool light_sleep_enabled;
    bool deep_sleep_enabled;
    uint32_t light_sleep_delay_seconds;
    uint32_t deep_sleep_delay_seconds;
} app_settings_snapshot_t;

typedef struct {
    size_t count;
    char names[APP_SETTINGS_MAX_AUTHORIZED_USERS]
              [APP_SETTINGS_MAX_USER_NAME_LENGTH];
} app_settings_authorized_users_t;

/**
 * Initialize persistent settings and load the saved values from NVS.
 * Defaults for a fresh device are light mode with every component enabled.
 */
esp_err_t app_settings_init(void);

/** Return a copy of the current in-memory settings. */
app_settings_snapshot_t app_settings_get(void);

/** Save the requested setting. Hardware policy is applied by Application Logic. */
esp_err_t app_settings_set_dark_mode(bool enabled);
esp_err_t app_settings_set_ethernet_enabled(bool enabled);
esp_err_t app_settings_set_wifi_enabled(bool enabled);
esp_err_t app_settings_set_camera_enabled(bool enabled);
esp_err_t app_settings_set_audio_enabled(bool enabled);
esp_err_t app_settings_set_sdcard_enabled(bool enabled);
esp_err_t app_settings_set_active_optimization_enabled(bool enabled);
esp_err_t app_settings_set_light_sleep_enabled(bool enabled);
esp_err_t app_settings_set_deep_sleep_enabled(bool enabled);

/** Persist a duration in the inclusive range 1..5999 seconds. */
esp_err_t app_settings_set_light_sleep_delay_seconds(uint32_t seconds);
esp_err_t app_settings_set_deep_sleep_delay_seconds(uint32_t seconds);


/**
 * Rescan /sdcard/enroll. Application Logic guarantees microSD availability.
 * Every call performs a fresh directory scan.
 */
esp_err_t app_settings_load_authorized_users(
    app_settings_authorized_users_t *users_out);

#ifdef __cplusplus
}
#endif
