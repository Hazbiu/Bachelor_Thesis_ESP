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
 * Suspend or resume application-owned hardware around manual Light-sleep.
 *
 * The suspend callback must stop every DMA/worker that cannot run while the
 * ESP32-P4 clocks are gated. The resume callback must rebuild those resources
 * after a touch or GPIO wake. A non-ESP_OK result is treated as unsafe and
 * causes a controlled restart instead of continuing with half-initialized
 * hardware.
 */
typedef esp_err_t (*app_sleep_light_transition_callback_t)(void *user_data);

/**
 * Register the application callbacks used around manual Light-sleep.
 *
 * Both callbacks are required. Registration is normally performed once during
 * app_main(), before app_sleep_start_timeout() starts the inactivity policy.
 */
esp_err_t app_sleep_register_light_sleep_callbacks(
    app_sleep_light_transition_callback_t suspend_callback,
    app_sleep_light_transition_callback_t resume_callback,
    void *user_data);

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
 * Stage 1: APP_LIGHT_SLEEP_TIMEOUT_MS without activity -> Light-sleep. Because
 * this display exposes GT911 only through I2C, the RTC wakes for short polling
 * slices. A detected touch restores display/camera without rebooting.
 * Stage 2: APP_DEEP_SLEEP_TIMEOUT_MS total inactivity -> Deep-sleep.
 *
 * GPIO3 also wakes Light-sleep back into the running application. If neither a
 * touch nor GPIO3 occurs, the polling slices end at the Deep-sleep deadline.
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
