#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool detector_valid;
    bool recognizer_valid;
    uint32_t detector_inference_us;
    uint32_t recognizer_inference_us;
} diagnostics_ai_live_metrics_t;

void diagnostics_start_ai_pipeline_monitor(void);

void diagnostics_ai_frame_sent_to_detector(void);
void diagnostics_ai_detection_result(int face_count);
void diagnostics_ai_recognition_result(
    esp_err_t ret,
    const char *name,
    float similarity);

/*
 * Live camera-overlay metrics.
 *
 * Detector/recognizer backends publish only the model invocation time here.
 * The camera renderer snapshots these values on CPU0 without depending on a
 * particular AI backend.
 */
void diagnostics_ai_live_metrics_reset(void);
void diagnostics_ai_live_metrics_record_detection(uint64_t inference_us);
void diagnostics_ai_live_metrics_record_recognition(uint64_t inference_us);
void diagnostics_ai_live_metrics_snapshot(
    diagnostics_ai_live_metrics_t *out_metrics);

#ifdef __cplusplus
}
#endif
