#include "face_recognition_wrapper.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG_RECOG = "face_recognition_wrapper";

extern "C" esp_err_t face_recognition_init(void)
{
    ESP_LOGI(TAG_RECOG, "Face recognition wrapper initialized");
    return ESP_OK;
}

extern "C" esp_err_t face_recognition_enroll(
    uint8_t *camera_buf,
    uint32_t width,
    uint32_t height,
    const face_box_t *box,
    const char *name)
{
    if (!camera_buf || !box || !name) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG_RECOG,
             "Enroll placeholder: name=%s box=[%d,%d,%d,%d] frame=%" PRIu32 "x%" PRIu32,
             name,
             box->x1, box->y1, box->x2, box->y2,
             width, height);

    return ESP_OK;
}

extern "C" esp_err_t face_recognition_recognize(
    uint8_t *camera_buf,
    uint32_t width,
    uint32_t height,
    const face_box_t *box,
    char *out_name,
    int out_name_len,
    float *out_score)
{
    if (!camera_buf || !box || !out_name || out_name_len <= 0 || !out_score) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(out_name, "unknown", out_name_len - 1);
    out_name[out_name_len - 1] = '\0';
    *out_score = 0.0f;

    ESP_LOGI(TAG_RECOG,
             "Recognize placeholder: box=[%d,%d,%d,%d] frame=%" PRIu32 "x%" PRIu32,
             box->x1, box->y1, box->x2, box->y2,
             width, height);

    return ESP_OK;
}