#pragma once

#include <stdbool.h>
#include <stdint.h>

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
* after a touchscreen wake. A non-ESP_OK result is treated as unsafe and
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
* Register the callback used before the ordered Deep-sleep shutdown.
*
* The function name is retained for existing callers. No button-monitor task
* is created. GPIO3 transitions do nothing in Active mode or Light-sleep.
* Immediately before real Deep-sleep, GPIO3 is debounced and the opposite
* level is armed. Either maintained rocker transition can then wake the P4.
*/
esp_err_t app_sleep_start_button_monitor(
    app_sleep_prepare_callback_t prepare_callback,
    void *user_data);

/**
* Start the NVS-selected face-inactivity power policy.
*
* Light ON + Deep ON:
*   Enter Light-sleep after the saved Light delay, then enter Deep-sleep
*   after the saved Deep delay from completed Light preparation if no activity wakes
*   the application back to Active mode.
*
* Light ON + Deep OFF:
*   Enter the SAME full Light-sleep hardware state after the saved Light delay.
*   The timer-sliced GT911 polling loop then continues indefinitely
*   until qualified touchscreen activity wakes the system.
*
* Light OFF + Deep ON:
*   Skip Light-sleep completely and run the existing ordered Deep-sleep
*   shutdown directly after the saved Deep delay of inactivity.
*
* Light OFF + Deep OFF:
*   Keep both automatic sleep transitions disabled while retaining the staged
*   Active-mode CPU policy.
*
* Positive face detections and existing application activity notifications
* reset the inactivity timestamp exactly as before.
*/
/** Set the saved Light/Deep policy supplied by Application Logic. */
void app_sleep_set_mode_policy(bool light_enabled, bool deep_enabled);

/**
 * Atomically stage the saved modes and durations (1..5999 seconds each).
 * Applied on the next Active policy tick with a fresh inactivity epoch; an
 * already running Light-sleep window retains its captured configuration.
 */
void app_sleep_set_policy(
    bool light_enabled, bool deep_enabled,
    uint32_t light_delay_seconds, uint32_t deep_delay_seconds);

esp_err_t app_sleep_start_timeout(void);

/**
 * Pause automatic Light/Deep sleep transitions while the application owns a
 * non-camera interaction state such as PIN entry. The inactivity task remains
 * alive, but it is not allowed to claim a Light- or Deep-sleep transition.
 */
bool app_sleep_pause_inactivity_policy(void);

/**
 * Resume the automatic inactivity policy and start a fresh inactivity epoch.
 * This is called only after the live camera/AI path is active again.
 */
void app_sleep_resume_inactivity_policy(void);

/** Return true while automatic sleep transitions are paused. */
bool app_sleep_inactivity_policy_is_paused(void);

/**
* Record camera/user activity and restart the inactivity epoch while the
* automatic policy is active.
*/
void app_sleep_notify_face_detected(void);

/**
* Return true only while the currently claimed automatic Light-sleep window
* is still justified by the inactivity timer. A long AI inference may finish
* after the Light-sleep deadline and report activity; callers use this check
* before destructively suspending camera/display hardware.
*/
bool app_sleep_light_sleep_is_due(void);

/**
 * Return true when the saved runtime policy has Deep-sleep enabled.
 *
 * Camera/display session code uses this only to select the Hybrid-safe panel
 * teardown strategy; the power service remains the source of truth for policy.
 */
bool app_sleep_deep_mode_is_enabled(void);

/** Request the existing ordered Deep-sleep shutdown sequence. */
void app_sleep_request(const char *reason);

/** Return true after a Deep-sleep request has been accepted. */
bool app_sleep_is_requested(void);

#ifdef __cplusplus
}
#endif
