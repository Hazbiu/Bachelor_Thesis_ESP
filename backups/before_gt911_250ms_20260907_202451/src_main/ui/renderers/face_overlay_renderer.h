#pragma once

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

#ifdef __cplusplus
}
#endif
