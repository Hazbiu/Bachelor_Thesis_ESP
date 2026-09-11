#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "services/vision/face_detector.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_RECOG_MAX_NAME_LEN 32

/*
 * Public recognition API used by app_main.c.
 *
 * ESP-DL backend:
 *   keeps the existing /sdcard/FACE.DB behavior.
 *
 * TFLM-FP32 backend:
 *   uses /sdcard/FACE_TFLM_FP32.DB and the same RGB enrollment folders under
 *   /sdcard/enroll/<person-name>/.
 */
esp_err_t face_recognition_init(void);

esp_err_t face_recognition_recognize(
    uint8_t *camera_buf,
    uint32_t width,
    uint32_t height,
    const face_box_t *box,
    char *out_name,
    int out_name_len,
    float *out_score
);

int face_recognition_get_count(void);
const char *face_recognition_backend_name(void);

#ifdef __cplusplus
}
#endif
