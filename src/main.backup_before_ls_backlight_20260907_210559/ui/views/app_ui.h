#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_ui_start_callback_t)(void *user_data);
typedef void (*app_ui_settings_callback_t)(void *user_data);

/** Create the modern launcher shown before the camera application starts. */
void app_ui_create(
    app_ui_start_callback_t start_callback,
    app_ui_settings_callback_t settings_callback,
    void *user_data);

/** Update the launcher status text from any FreeRTOS task. */
void app_ui_set_status(const char *text);

/** Show a startup failure on the launcher screen. */
void app_ui_show_error(const char *text);

/** Remove launcher objects before switching to Settings or camera mode. */
void app_ui_destroy(void);

#ifdef __cplusplus
}
#endif
