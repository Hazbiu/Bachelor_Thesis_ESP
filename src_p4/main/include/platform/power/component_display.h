#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Put the display side-channels into their lowest software-controlled state
 * before ESP32-P4 Deep-sleep.
 *
 * V19 uses the GT911 full-Sleep command (0x05 -> 0x8040), not automatic
 * Green mode. The controller is considered asleep only when it stops ACKing
 * on I2C while another device on the same bus (ES8311) still responds.
 *
 * IMPORTANT: on the stock ESP32-P4-NANO BSP, GT911 INT and RESET are not
 * connected to a P4 GPIO. Goodix requires INT-high or RESET to wake from full
 * Sleep. Therefore this V19 build is intended for deepest-software current
 * measurement: after GPIO3 wakes the P4, a complete board power-cycle is
 * required before the GT911/touch path can operate again.
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

/**
 * Audit the selected GT911 low-power state while the shared I2C bus is still
 * available. In V19 this verifies full Sleep by confirming:
 *   - ES8311 still ACKs (bus healthy);
 *   - the sleeping GT911 does not ACK.
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
 * On stock wiring with V19 full-Sleep enabled this returns
 * ESP_ERR_NOT_SUPPORTED and logs that a full board power-cycle is required.
 */
esp_err_t component_display_wake_touch_after_reset(void);

#ifdef __cplusplus
}
#endif
