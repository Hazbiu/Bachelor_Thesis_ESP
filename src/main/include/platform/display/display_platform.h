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

/*
 * Put the physical LCD controller into DISPLAY_OFF + SLEEP_IN while the BSP
 * panel handle still exists. If a Hybrid Light->Deep transition already issued
 * this command before tearing DSI down, a later Deep-sleep call is idempotent.
 */
esp_err_t display_platform_panel_enter_full_sleep(void);
bool display_platform_panel_sleep_committed(void);

/*
 * Suspend the BSP display transport for Light-sleep.
 *
 * prepare_panel_for_deep=true is used only by the Hybrid policy. It sends the
 * LCD controller's strongest software sleep command before the Light-sleep
 * teardown destroys the panel handle, so the later Deep-sleep phase cannot
 * lose that power-saving step.
 */
esp_err_t display_platform_suspend_for_light_sleep(
    bool prepare_panel_for_deep);

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
