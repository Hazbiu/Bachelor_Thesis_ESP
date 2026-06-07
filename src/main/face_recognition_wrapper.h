#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "face_detect_wrapper.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_RECOG_MAX_NAME_LEN 32

esp_err_t face_recognition_init(void);

esp_err_t face_recognition_enroll(
    uint8_t *camera_buf,
    uint32_t width,
    uint32_t height,
    const face_box_t *box,
    const char *name
);

esp_err_t face_recognition_recognize(
    uint8_t *camera_buf,
    uint32_t width,
    uint32_t height,
    const face_box_t *box,
    char *out_name,
    int out_name_len,
    float *out_score
);

#ifdef __cplusplus
}
#endif