#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Called immediately before the application begins the destructive
 * Deep-sleep shutdown sequence.
 *
 * The application should stop new camera-frame, PPA, AI and display work.
 * Light-sleep does not use this callback because execution resumes in place.
 */
typedef void (*app_sleep_prepare_callback_t)(void *user_data);

/**
 * Start the physical Deep-sleep button monitor.
 *
 * GPIO3 remains the Deep-sleep button while the application is active. When
 * GPIO3 is used only to wake an automatic Light-sleep interval, that wake
 * press is consumed and will not immediately request Deep-sleep.
 */
esp_err_t app_sleep_start_button_monitor(
    app_sleep_prepare_callback_t prepare_callback,
    void *user_data);

/**
 * Start the two-stage face-inactivity power policy.
 *
 * Stage 1: APP_LIGHT_SLEEP_TIMEOUT_MS without activity -> Light-sleep.
 * Stage 2: APP_DEEP_SLEEP_TIMEOUT_MS total inactivity -> Deep-sleep.
 *
 * GPIO3 wakes Light-sleep back into the running application. The remaining
 * time to the Deep-sleep deadline is also installed as a Light-sleep timer.
 */
esp_err_t app_sleep_start_timeout(void);

/**
 * Record user activity and restart the complete inactivity policy.
 *
 * Positive face detections and PIN-screen interaction both call this API.
 */
void app_sleep_notify_face_detected(void);

/** Request the existing ordered Deep-sleep shutdown sequence. */
void app_sleep_request(const char *reason);

/** Return true after a Deep-sleep request has been accepted. */
bool app_sleep_is_requested(void);

#ifdef __cplusplus
}
#endif
