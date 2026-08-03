#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Called immediately before the application begins hardware shutdown.
 *
 * The application should stop new camera-frame, PPA, AI and display work.
 */
typedef void (*app_sleep_prepare_callback_t)(void *user_data);

/**
 * Start the physical deep-sleep button monitor.
 *
 * The button is configured as an active-low input with its internal pull-up
 * enabled. A callback is invoked before the display and camera are shut down.
 */
esp_err_t app_sleep_start_button_monitor(
    app_sleep_prepare_callback_t prepare_callback,
    void *user_data);

/**
 * Start the face-inactivity monitor.
 *
 * The initial timeout begins when this function is called. Every subsequent
 * call to app_sleep_notify_face_detected() restarts the complete timeout.
 */
esp_err_t app_sleep_start_timeout(void);

/**
 * Record a positive face-detection result and restart the inactivity window.
 *
 * Known and unknown faces are both activity. Calls made before the inactivity
 * monitor starts, or after a sleep request is accepted, are ignored.
 */
void app_sleep_notify_face_detected(void);

/**
 * Request an ordered transition into deep sleep.
 *
 * Duplicate requests are ignored.
 */
void app_sleep_request(const char *reason);

/**
 * Return true after a deep-sleep request has been accepted.
 */
bool app_sleep_is_requested(void);

#ifdef __cplusplus
}
#endif
