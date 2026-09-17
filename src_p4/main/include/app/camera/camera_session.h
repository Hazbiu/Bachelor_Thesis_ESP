#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Application-level live camera/display session.
 *
 * Owns the stateful coordination that must remain ordered across:
 * - launcher display ownership;
 * - camera/PPA/display buffers;
 * - CPU1 Vision worker startup;
 * - Authentication Flow context;
 * - reversible Light-sleep and idle-scan transitions;
 * - Power Manager callback ports;
 * - live frame rendering and AI snapshot scheduling.
 *
 * Physical drivers remain behind platform adapters and application state
 * transitions remain in the App Controller/FSM.
 */
esp_err_t camera_session_prepare_launcher_display(void);

bool camera_session_launcher_touch_ready(void);

void camera_session_enable_launcher_backlight(void);

void camera_session_launcher_start_requested(void *user_data);



/*
 * Deep-sleep wake fast path.
 *
 * Prepares the display stack without creating or showing launcher widgets, then
 * starts the normal camera/AI initialization path in serial-only startup mode.
 * deep_wake_start_us is captured at app-runtime entry and is used only for
 * software wake-latency measurement (ROM/bootloader time is intentionally not
 * included because esp_timer resets across real Deep-sleep).
 */
esp_err_t camera_session_prepare_direct_wake_display(void);
void camera_session_start_direct_after_deep_sleep(int64_t deep_wake_start_us);

esp_err_t camera_session_setup_power_management(void);

#ifdef __cplusplus
}
#endif
