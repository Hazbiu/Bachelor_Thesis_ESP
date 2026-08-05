#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_ui_start_callback_t)(void *user_data);

/** Create the launcher screen shown before the camera application starts. */
void app_ui_create(app_ui_start_callback_t start_callback, void *user_data);

/** Update the launcher status text from any FreeRTOS task. */
void app_ui_set_status(const char *text);

/** Show a startup failure on the launcher screen. */
void app_ui_show_error(const char *text);

/** Remove launcher objects before switching the display to camera dummy-draw mode. */
void app_ui_destroy(void);

#ifdef __cplusplus
}
#endif
