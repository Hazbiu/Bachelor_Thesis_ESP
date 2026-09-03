#pragma once

#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif
