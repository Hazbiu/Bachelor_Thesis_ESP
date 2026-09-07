#pragma once

#include "esp_err.h"

void diagnostics_start_ai_pipeline_monitor(void);

void diagnostics_ai_frame_sent_to_detector(void);
void diagnostics_ai_detection_result(int face_count);
void diagnostics_ai_recognition_result(esp_err_t ret, const char *name, float similarity);