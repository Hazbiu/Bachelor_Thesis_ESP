#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "services/vision/face_detector.h"
#include "services/vision/face_recognizer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Existing ESP-DL backend. */
esp_err_t espdl_face_recognition_init(void);
esp_err_t espdl_face_recognition_recognize(
    uint8_t *camera_buf, uint32_t width, uint32_t height,
    const face_box_t *box, char *out_name, int out_name_len, float *out_score);
int espdl_face_recognition_get_count(void);

/* MobileFaceNet FP32 running through TensorFlow Lite Micro. */
esp_err_t tflm_fp32_face_recognition_init(void);
esp_err_t tflm_fp32_face_recognition_recognize(
    uint8_t *camera_buf, uint32_t width, uint32_t height,
    const face_box_t *box, char *out_name, int out_name_len, float *out_score);
int tflm_fp32_face_recognition_get_count(void);

/* MobileFaceNet full-INT8 running through TFLM + ESP-NN. */
esp_err_t tflm_int8_face_recognition_init(void);
esp_err_t tflm_int8_face_recognition_recognize(
    uint8_t *camera_buf, uint32_t width, uint32_t height,
    const face_box_t *box, char *out_name, int out_name_len, float *out_score);
int tflm_int8_face_recognition_get_count(void);

#ifdef __cplusplus
}
#endif
