#pragma once

#include <stdint.h>

#include "services/vision/face_detector.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convert detector boxes from AI-snapshot coordinates to source-camera
 * coordinates, apply backend-specific display-box policy, log the resulting
 * boxes, and publish the immediate red-box result.
 *
 * Returns the number of boxes accepted for downstream recognition, clamped to
 * APP_MAX_FACE_BOXES exactly as before.
 *
 * This service does not control sleep, CPU frequency, FSM transitions,
 * recognition, authentication, worker state or PIN transitions.
 */
int vision_face_detection_result_process_and_publish(
    const face_box_t *snapshot_boxes,
    int face_count,
    uint32_t snapshot_width,
    uint32_t snapshot_height,
    uint32_t source_width,
    uint32_t source_height);

#ifdef __cplusplus
}
#endif
