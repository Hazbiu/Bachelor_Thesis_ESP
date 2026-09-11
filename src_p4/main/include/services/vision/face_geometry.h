#pragma once

#include <stdint.h>

#include "services/vision/face_detector.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convert a face box from AI snapshot coordinates back to the
 * original camera-frame coordinate system.
 *
 * Box coordinates and detector keypoints are scaled identically
 * to the previous app_main implementation.
 */
void vision_face_geometry_scale_box_to_source(
    const face_box_t *snapshot_box,
    uint32_t snapshot_width,
    uint32_t snapshot_height,
    uint32_t source_width,
    uint32_t source_height,
    face_box_t *source_box);

float vision_face_geometry_iou(
    const face_box_t *a,
    const face_box_t *b);

int vision_face_geometry_smooth_coordinate(
    int old_value,
    int new_value);

/*
 * Expand a source/display face rectangle by a margin ratio while
 * keeping the resulting coordinates inside the source frame.
 *
 * This affects only the display rectangle. Detector keypoints and
 * the recognition input box are not modified.
 */
void vision_face_geometry_expand_box_for_display(
    face_box_t *box,
    uint32_t source_width,
    uint32_t source_height,
    float margin_ratio);

#ifdef __cplusplus
}
#endif
