#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hide optional LVGL performance and memory overlays before direct camera
 * rendering takes ownership of the display.
 *
 * The caller retains ownership of the LVGL display handle.
 */
void display_platform_disable_lvgl_overlays(
    lv_display_t *display);

/*
 * Physical LCD backlight adapter.
 *
 * Application code keeps its existing software shadow state; this platform
 * layer only hides the BSP-specific backlight API.
 */
void display_platform_backlight_on(void);
void display_platform_backlight_off(void);

/*
 * BSP display lifecycle adapter.
 *
 * Application code remains responsible for delays, display-buffer allocation,
 * error recovery, sleep state and UI sequencing.
 */
lv_display_t *display_platform_start(void);
esp_err_t display_platform_suspend_for_light_sleep(void);
lv_display_t *display_platform_resume_from_light_sleep(void);
lv_indev_t *display_platform_get_input_device(void);

/* Poll the BSP-owned GT911 path used by retained Light-sleep wake logic. */
esp_err_t display_platform_poll_touch_for_light_sleep(bool *touched);

/* Board display geometry exposed without leaking BSP macros upward. */
uint32_t display_platform_width(void);
uint32_t display_platform_height(void);

#ifdef __cplusplus
}
#endif
