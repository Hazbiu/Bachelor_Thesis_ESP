#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x1;
    int y1;
    int x2;
    int y2;
    float score;
    int keypoints[10];
    int keypoint_count;
} face_box_t;

/* Public API used by app_main.c. The selected backend is hidden underneath. */
esp_err_t face_detect_init(void);

int face_detect_run_rgb565(
    uint8_t *buf,
    uint32_t width,
    uint32_t height,
    face_box_t *boxes,
    int max_boxes
);

int face_detect_run_rgb888(
    uint8_t *buf,
    uint32_t width,
    uint32_t height,
    face_box_t *boxes,
    int max_boxes
);

const char *face_detect_backend_name(void);

#ifdef __cplusplus
}
#endif
