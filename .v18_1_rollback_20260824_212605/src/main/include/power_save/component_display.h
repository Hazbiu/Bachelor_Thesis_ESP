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
 *   1. preserves the BSP-controlled backlight-off state;
 *   2. on the stock board, configures GT911 Low_Power_Control (0x8055) for
 *      automatic Green mode, which self-wakes on touch;
 *   3. only on a future hardware revision with verified INT/RESET control,
 *      optionally uses the stronger GT911 full-Sleep command and pin holds.
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
 * Verify the safe GT911 software-only Deep-sleep preparation while the shared
 * I2C bus is still available.
 *
 * On the stock ESP32-P4-NANO, full GT911 Sleep remains disabled because no
 * host-accessible INT/RESET wake pin exists. V18 instead configures the GT911
 * automatic Green/low-power idle interval and verifies register 0x8055.
 */
esp_err_t component_display_verify_deep_sleep_low_power(void);

/**
 * Release the display side-channel holds only when entering Deep-sleep
 * unexpectedly fails.
 */
esp_err_t component_display_restore_after_failed_sleep(void);

/**
 * Bring the GT911 out of sleep after an ESP32-P4 reset.
 *
 * The touch controller runs from the display module's always-on 3.3 V rail,
 * so the sleep command written before Deep-sleep survives a Deep-sleep wake,
 * an EN-pin reset and a reflash. Its I2C interface is off while it sleeps, so
 * an unwoken controller makes the BSP abort at boot:
 *
 *     bsp_display_indev_init -> bsp_touch_new() -> ESP_ERR_NOT_FOUND
 *
 * This routine performs the hardware wake - a RESET toggle when that pin is
 * known, otherwise an INT pulse - and must therefore run BEFORE
 * bsp_display_start().
 *
 * It is registered as a constructor in component_display.c so it runs ahead of
 * app_main() without any edit to app_main.c, and it is exported here so it can
 * be called explicitly instead if you prefer that to be visible in app_main().
 * Calling it twice is harmless.
 *
 * With no touch pin configured the whole thing compiles to nothing: sleeping
 * the GT911 is refused at build time in that case, so there is never anything
 * to wake.
 */
esp_err_t component_display_wake_touch_after_reset(void);

#ifdef __cplusplus
}
#endif
