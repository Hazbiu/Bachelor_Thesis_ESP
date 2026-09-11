#pragma once

#include <stdbool.h>

#include "services/vision/face_detector.h"
#include "services/vision/face_recognizer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Thread-safe latest-result store shared by the asynchronous AI worker
 * and the CPU0 camera/display path.
 *
 * There is intentionally no result queue. The store always represents
 * only the most recently published face state.
 */

void vision_face_result_store_clear(void);

void vision_face_result_store_publish_detected_boxes(
    const face_box_t *boxes,
    int count,
    bool preserve_previous_names);

void vision_face_result_store_publish_recognition_result(
    int index,
    const char *name,
    float score);

/*
 * Record one detector pass with no face.
 *
 * Returns true once APP_FACE_BOX_HOLD_MISSES has been reached. The caller
 * retains ownership of the CPU-frequency policy so the vision service does
 * not depend on platform/power.
 */
bool vision_face_result_store_note_no_face(void);

/*
 * Atomically copy the latest result.
 *
 * Any output pointer may be NULL.
 * Returns the number of valid faces copied.
 */
int vision_face_result_store_snapshot(
    face_box_t *boxes,
    char names[][FACE_RECOG_MAX_NAME_LEN],
    float *scores,
    int capacity);

#ifdef __cplusplus
}
#endif
