#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called after the four-digit PIN has been verified successfully. */
typedef void (*pin_screen_success_callback_t)(void *user_data);

/**
 * Show the second-factor PIN screen.
 *
 * The caller must switch the display from direct camera drawing back to normal
 * LVGL drawing before calling this function. The recognized name is copied by
 * this module and may therefore point to temporary storage.
 */
esp_err_t pin_screen_show(
    const char *recognized_name,
    pin_screen_success_callback_t success_callback,
    void *user_data);

/** Remove all PIN-screen objects. Safe to call when the screen is not visible. */
void pin_screen_hide(void);

/** Return true while the PIN screen owns the active LVGL screen. */
bool pin_screen_is_visible(void);

#ifdef __cplusplus
}
#endif
