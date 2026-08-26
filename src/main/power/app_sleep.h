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
* Start the physical GPIO3 sleep-button monitor.
*
* When the saved Deep-sleep toggle is ON, GPIO3 remains an active-mode
* Deep-sleep request and the Deep-sleep wake pin. When it is OFF, GPIO3 is only
* a Light-sleep wake/activity input.
*
* A GPIO3 press used to wake Light-sleep is consumed and will not be reused as
* a second sleep request after the system resumes.
*/
esp_err_t app_sleep_start_button_monitor(
    app_sleep_prepare_callback_t prepare_callback,
    void *user_data);

/**
* Start the NVS-selected face-inactivity power policy.
*
* Light ON + Deep ON:
*   Enter Light-sleep at APP_LIGHT_SLEEP_TIMEOUT_MS, then enter Deep-sleep
*   APP_POWER_MODES_LIGHT_TO_DEEP_GAP_MS later if no touch/GPIO3 activity wakes
*   the application back to Active mode.
*
* Light ON + Deep OFF:
*   Enter reversible Light-sleep after APP_SINGLE_SLEEP_TIMEOUT_MS (7 s).
*   The timer-sliced GT911 polling loop continues indefinitely until qualified
*   touchscreen activity or GPIO3 wakes the system.
*
* Light OFF + Deep ON:
*   Skip Light-sleep completely and run the existing ordered Deep-sleep
*   shutdown directly after APP_SINGLE_SLEEP_TIMEOUT_MS (7 s) inactivity.
*
* Light OFF + Deep OFF:
*   Keep both automatic sleep transitions disabled while retaining the staged
*   Active-mode CPU policy.
*
* Positive face detections and existing application activity notifications
* reset the inactivity timestamp exactly as before.
*/
esp_err_t app_sleep_start_timeout(void);

/**
* Record user activity and restart the complete inactivity policy.
*
* Positive face detections and PIN-screen interaction both call this API.
*/
void app_sleep_notify_face_detected(void);

/**
* Return true only while the currently claimed automatic Light-sleep window
* is still justified by the inactivity timer. A long AI inference may finish
* after the Light-sleep deadline and report activity; callers use this check
* before destructively suspending camera/display hardware.
*/
bool app_sleep_light_sleep_is_due(void);

/** Request the existing ordered Deep-sleep shutdown sequence. */
void app_sleep_request(const char *reason);

/** Return true after a Deep-sleep request has been accepted. */
bool app_sleep_is_requested(void);

#ifdef __cplusplus
}
#endif
