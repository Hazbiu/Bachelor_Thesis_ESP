#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decide whether a detector-approved face candidate should proceed
 * to the recognition stage.
 *
 * The caller provides the configured recognition threshold so this
 * service does not own application-level tuning values.
 *
 * Backend-specific behavior is preserved:
 *
 *   ESP-DL / TFLM FP32:
 *       detector_score > minimum_score
 *
 *   TFLM INT8:
 *       every candidate already accepted by the quantized detector
 *       proceeds to recognition.
 */
bool vision_recognition_policy_allows(
    float detector_score,
    float minimum_score);

/*
 * Decide whether this detector-positive frame should run recognition.
 *
 * Backend-specific behavior is exactly the former app_main policy:
 *
 *   TFLM INT8:
 *       every detector-positive frame proceeds to recognition.
 *
 *   ESP-DL / TFLM FP32:
 *       recognition runs when frame_id % interval_frames == 0.
 *
 * The caller must provide the same non-zero configured interval used before.
 */
bool vision_recognition_policy_should_run_frame(
    uint32_t frame_id,
    uint32_t interval_frames);

#ifdef __cplusplus
}
#endif
