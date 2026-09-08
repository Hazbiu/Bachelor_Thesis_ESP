#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Normalize one completed recognizer result and publish it to the Vision
 * face-result store.
 *
 * Exact existing behavior is preserved:
 * - successful recognition keeps the recognizer-provided name and score;
 * - failed recognition becomes name="unknown" and score=0.0f;
 * - the normalized result is published for the supplied face index.
 *
 * Raw diagnostics remain the caller's responsibility and occur before this
 * function. Authentication, FSM transitions, CPU locks, sleep, worker state,
 * and PIN transitions remain outside this service.
 */
void vision_face_recognition_result_normalize_and_publish(
    int face_index,
    esp_err_t recognition_result,
    char *name,
    size_t name_capacity,
    float *score);

#ifdef __cplusplus
}
#endif
