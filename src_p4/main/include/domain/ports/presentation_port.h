#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*presentation_action_callback_t)(void *user_data);

void presentation_launcher_create(
    presentation_action_callback_t start_callback,
    presentation_action_callback_t settings_callback,
    void *user_data);
void presentation_launcher_set_status(const char *text);
void presentation_launcher_show_error(const char *text);
void presentation_launcher_destroy(void);

void presentation_settings_create(
    presentation_action_callback_t back_callback,
    void *user_data);
void presentation_settings_destroy(void);

esp_err_t presentation_pin_show(
    const char *recognized_name,
    presentation_action_callback_t success_callback,
    void *user_data);
void presentation_pin_hide(void);
bool presentation_pin_is_visible(void);

void presentation_face_overlay_draw_box_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int x1,
    int y1,
    int x2,
    int y2,
    int thickness,
    uint16_t color);

void presentation_face_overlay_draw_label_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int box_x2,
    int box_y1,
    const char *name,
    uint16_t background_color);

#ifdef __cplusplus
}
#endif
