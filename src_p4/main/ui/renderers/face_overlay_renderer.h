#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void face_overlay_renderer_draw_box_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int x1,
    int y1,
    int x2,
    int y2,
    int thickness,
    uint16_t color);

void face_overlay_renderer_draw_label_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int box_x2,
    int box_y1,
    const char *name,
    uint16_t background_color);

/*
 * Draw live AI/camera/HP-Core metrics in a compact panel at the upper-right
 * corner of the RGB565 camera preview.
 */
void face_overlay_renderer_draw_metrics_rgb565(
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
    uint32_t hp_core1_usage_x10);

#ifdef __cplusplus
}
#endif
