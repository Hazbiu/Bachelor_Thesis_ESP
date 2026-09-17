#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Put the display side-channels into their lowest software-controlled state
 * before ESP32-P4 Deep-sleep.
 *
 * Light-sleep keeps the Goodix controller pollable. Real Deep-sleep instead
 * asserts the Waveshare panel-MCU TS_RESET output, so the GT9271 stops scanning
 * and cannot raise current when the glass is touched. GPIO3 remains the only
 * P4 wake source; the reset is released before BSP touch probing after wake.
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
 * Audit the selected Deep-sleep touch state while the shared I2C bus is still
 * available. On this board the final audit verifies panel-MCU TS_RESET remains
 * asserted; optional legacy Goodix Green/full-Sleep paths remain compile-time
 * fallbacks only.
 */
esp_err_t component_display_verify_deep_sleep_low_power(void);

/**
 * Release display-side holds/reset only if Deep-sleep unexpectedly returns.
 * The panel-MCU touch reset is recoverable in software, so failed entry does
 * not strand the touchscreen asleep.
 */
esp_err_t component_display_restore_after_failed_sleep(void);

/**
 * Compatibility wake entry point. Direct P4 RESET/INT is still supported when
 * present; on the stock board the display-platform path releases the panel-MCU
 * TS_RESET output after a GPIO3 Deep-sleep wake.
 */
esp_err_t component_display_wake_touch_after_reset(void);

#ifdef __cplusplus
}
#endif
