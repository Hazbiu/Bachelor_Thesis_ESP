
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Put the display side-channels into their lowest software-controlled state
 * before ESP32-P4 Deep-sleep.
 *
 * The default policy uses automatic GT911 Green mode. Full Sleep stays
 * disabled because the stock board has no host INT/RESET wake connection.
 * This preserves touch availability after the P4 wakes through GPIO3.
 *
 * Call ordering:
 *
 *   component_display_panel_enter_full_sleep()
 *   bsp_display_shutdown_for_deep_sleep()
 *   component_display_disable_for_deep_sleep()
 *   ... remaining shutdown stages ...
 *   enter_deep_sleep()
 */

/**
 * Ensure the active LCD controller has received full DCS Sleep-In.
 *
 * Direct Deep-sleep:
 *   sends backlight OFF -> DISPLAY_OFF (0x28) -> SLEEP_IN (0x10) while the
 *   panel handle is still alive.
 *
 * Hybrid Light->Deep:
 *   the display platform sends SLEEP_IN before the Light-sleep teardown
 *   destroys the panel handle. This call is then idempotent and accepts the
 *   previously committed panel-sleep state.
 */
esp_err_t component_display_panel_enter_full_sleep(void);

esp_err_t component_display_disable_for_deep_sleep(void);

/* Call with BSP touch/UI polling suspended and shared I2C still available.
 * Shortens the verified GT911 automatic Green idle interval, at most once per
 * configuration change. Normal scanning resumes on touch; full Sleep is not
 * used. Unsupported controller layouts are left untouched. */
esp_err_t component_display_prepare_touch_for_sleep(void);

/**
 * Audit the selected GT911 low-power state while the shared I2C bus is still
 * available. The default policy verifies the automatic Green configuration;
 * this does not measure whether the controller is currently scanning slowly.
 * The optional full-Sleep build instead checks I2C non-response and bus health.
 */
esp_err_t component_display_verify_deep_sleep_low_power(void);

/**
 * Release display-side GPIO holds only if Deep-sleep unexpectedly returns.
 * A GT911 already placed into full Sleep cannot be software-restored on the
 * stock board because no host INT/RESET line is available.
 */
esp_err_t component_display_restore_after_failed_sleep(void);

/**
 * Wake GT911 when a future board revision provides a usable RESET or INT GPIO.
 * On stock wiring with full-Sleep enabled this returns
 * ESP_ERR_NOT_SUPPORTED and logs that a full board power-cycle is required.
 */
esp_err_t component_display_wake_touch_after_reset(void);

#ifdef __cplusplus
}
#endif
