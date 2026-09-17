#include "domain/ports/presentation_port.h"

#include "renderers/face_overlay_renderer.h"
#include "views/app_ui.h"
#include "views/pin_screen.h"
#include "views/settings_screen.h"

void presentation_launcher_create(
    presentation_action_callback_t start_callback,
    presentation_action_callback_t settings_callback,
    void *user_data)
{
    app_ui_create(start_callback, settings_callback, user_data);
}

void presentation_launcher_set_status(const char *text)
{
    app_ui_set_status(text);
}

void presentation_launcher_show_error(const char *text)
{
    app_ui_show_error(text);
}

void presentation_launcher_destroy(void)
{
    app_ui_destroy();
}

void presentation_settings_create(
    presentation_action_callback_t back_callback,
    void *user_data)
{
    settings_screen_create(back_callback, user_data);
}

void presentation_settings_destroy(void)
{
    settings_screen_destroy();
}

esp_err_t presentation_pin_show(
    const char *recognized_name,
    presentation_action_callback_t success_callback,
    void *user_data)
{
    return pin_screen_show(recognized_name, success_callback, user_data);
}

void presentation_pin_hide(void)
{
    pin_screen_hide();
}

bool presentation_pin_is_visible(void)
{
    return pin_screen_is_visible();
}

void presentation_face_overlay_draw_box_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int x1,
    int y1,
    int x2,
    int y2,
    int thickness,
    uint16_t color)
{
    face_overlay_renderer_draw_box_rgb565(
        fb, fb_w, fb_h, x1, y1, x2, y2, thickness, color);
}

void presentation_face_overlay_draw_label_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int box_x2,
    int box_y1,
    const char *name,
    uint16_t background_color)
{
    face_overlay_renderer_draw_label_rgb565(
        fb, fb_w, fb_h, box_x2, box_y1, name, background_color);
}

void presentation_camera_metrics_draw_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    bool detector_valid,
    uint32_t detector_inference_us,
    bool recognizer_valid,
    uint32_t recognizer_inference_us,
    uint32_t fps_x10,
    bool hp_cpu_usage_valid,
    uint32_t hp_core0_usage_x10,
    uint32_t hp_core1_usage_x10)
{
    face_overlay_renderer_draw_metrics_rgb565(
        fb,
        fb_w,
        fb_h,
        detector_valid,
        detector_inference_us,
        recognizer_valid,
        recognizer_inference_us,
        fps_x10,
        hp_cpu_usage_valid,
        hp_core0_usage_x10,
        hp_core1_usage_x10);
}
