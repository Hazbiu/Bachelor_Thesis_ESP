#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "services/vision/face_detector.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Existing ESP-DL backend. */
esp_err_t espdl_face_detect_init(void);
int espdl_face_detect_run_rgb565(
    uint8_t *buf, uint32_t width, uint32_t height,
    face_box_t *boxes, int max_boxes);
int espdl_face_detect_run_rgb888(
    uint8_t *buf, uint32_t width, uint32_t height,
    face_box_t *boxes, int max_boxes);

/* BlazeFace FP32 running through TensorFlow Lite Micro. */
esp_err_t tflm_fp32_face_detect_init(void);
int tflm_fp32_face_detect_run_rgb565(
    uint8_t *buf, uint32_t width, uint32_t height,
    face_box_t *boxes, int max_boxes);
int tflm_fp32_face_detect_run_rgb888(
    uint8_t *buf, uint32_t width, uint32_t height,
    face_box_t *boxes, int max_boxes);

/* BlazeFace full-INT8 running through TFLM + ESP-NN. */
esp_err_t tflm_int8_face_detect_init(void);
int tflm_int8_face_detect_run_rgb565(
    uint8_t *buf, uint32_t width, uint32_t height,
    face_box_t *boxes, int max_boxes);
int tflm_int8_face_detect_run_rgb888(
    uint8_t *buf, uint32_t width, uint32_t height,
    face_box_t *boxes, int max_boxes);

#ifdef __cplusplus
}
#endif
