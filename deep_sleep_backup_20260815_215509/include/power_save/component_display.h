#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Put the display side-channels into their lowest state before ESP32-P4
 * Deep-sleep.
 *
 * The BSP call bsp_display_shutdown_for_deep_sleep() removes LVGL, the MIPI
 * DSI panel and the touch driver, but it does not tell the GT911 controller
 * itself to stop scanning, and it does not guarantee that the backlight-enable
 * pad keeps its off level once the digital domain is powered down.
 *
 * This function therefore performs the remaining work:
 *
 *   1. drives and holds the backlight-enable pin at its off level;
 *   2. sends the GT911 sleep command (0x05 -> register 0x8040) over the
 *      shared I2C bus;
 *   3. drives and holds the GT911 INT pin LOW so the controller cannot wake
 *      itself, and optionally asserts and holds the GT911 RESET pin.
 *
 * Call ordering is important:
 *
 *   bsp_display_shutdown_for_deep_sleep()   <- BSP releases its touch handle
 *   component_display_disable_for_deep_sleep()
 *   ... remaining shutdown stages ...
 *   enter_deep_sleep()                      <- isolates the shared I2C pins
 *
 * Running before the BSP shutdown risks the BSP's own touch teardown waking
 * the controller again; running after the I2C isolation makes the I2C write
 * impossible.
 *
 * A missing I2C bus or an unresponsive controller is reported but never fatal:
 * failing to sleep the touch panel must not prevent the system from sleeping.
 */
esp_err_t component_display_disable_for_deep_sleep(void);

/**
 * Release the display side-channel holds only when entering Deep-sleep
 * unexpectedly fails.
 */
esp_err_t component_display_restore_after_failed_sleep(void);

#ifdef __cplusplus
}
#endif
