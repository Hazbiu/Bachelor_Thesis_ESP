#include "services/vision/face_recognizer.h"

#include <stdio.h>
#include <string.h>
#include <list>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dl_detect_define.hpp"
#include "dl_image_define.hpp"
#include "human_face_recognition.hpp"

#include "diagnostics/core_trace.h"
#include "platform/camera/video_capture.h"

static const char *TAG_RECOG = "face_recognition_wrapper";

static HumanFaceRecognizer *s_recognizer = nullptr;

#define FACE_DATABASE_PATH "/sdcard/FACE.DB"
#define FACE_PERSON_NAME   "person_1"

/*
 * Recognition threshold.
 *
 * Increase this value if wrong people are recognized as the same person.
 * Decrease slightly if the correct person is often rejected.
 *
 * Good test values: 0.65, 0.70, 0.75
 */
#define FACE_SIMILARITY_THRESHOLD 0.55f

static std::list<dl::detect::result_t> make_detect_result_from_box(const face_box_t *box)
{
    std::list<dl::detect::result_t> detect_res;

    dl::detect::result_t r = {};
    r.category = 0;
    r.score = box->score;
    r.box = {box->x1, box->y1, box->x2, box->y2};

    int w = box->x2 - box->x1;
    int h = box->y2 - box->y1;

    int left_eye_x  = box->x1 + w * 35 / 100;
    int right_eye_x = box->x1 + w * 65 / 100;
    int eye_y       = box->y1 + h * 38 / 100;
    int nose_x      = box->x1 + w * 50 / 100;
    int nose_y      = box->y1 + h * 55 / 100;
    int mouth_l_x   = box->x1 + w * 38 / 100;
    int mouth_r_x   = box->x1 + w * 62 / 100;
    int mouth_y     = box->y1 + h * 75 / 100;

    r.keypoint = {
        left_eye_x,  eye_y,
        right_eye_x, eye_y,
        nose_x,      nose_y,
        mouth_l_x,   mouth_y,
        mouth_r_x,   mouth_y
    };

    detect_res.push_back(r);
    return detect_res;
}

static dl::image::img_t make_camera_image(
    uint8_t *camera_buf,
    uint32_t width,
    uint32_t height)
{
    dl::image::img_t img = {
        .data = camera_buf,
        .width = (uint16_t)width,
        .height = (uint16_t)height,
#if APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
#else
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
#endif
    };

    return img;
}

extern "C" esp_err_t face_recognition_init(void)
{
    if (s_recognizer) {
        return ESP_OK;
    }

    s_recognizer = new HumanFaceRecognizer(FACE_DATABASE_PATH);

    if (!s_recognizer) {
        ESP_LOGE(TAG_RECOG, "Failed to create HumanFaceRecognizer");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG_RECOG,
             "HumanFaceRecognizer loaded %s, database_features=%d",
             FACE_DATABASE_PATH,
             s_recognizer->get_num_feats());

    return ESP_OK;
}

extern "C" int face_recognition_get_count(void)
{
    if (!s_recognizer) {
        return 0;
    }

    return s_recognizer->get_num_feats();
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
    if (!s_recognizer) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!camera_buf || !box || !out_name || out_name_len <= 0 || !out_score) {
        return ESP_ERR_INVALID_ARG;
    }

    out_name[0] = '\0';
    *out_score = 0.0f;

    if (s_recognizer->get_num_feats() <= 0) {
        strncpy(out_name, "unknown", out_name_len - 1);
        out_name[out_name_len - 1] = '\0';

        ESP_LOGW(TAG_RECOG,
                 "Recognition skipped: no features in %s",
                 FACE_DATABASE_PATH);

        return ESP_OK;
    }

    dl::image::img_t img = make_camera_image(
        camera_buf,
        width,
        height
    );

    std::list<dl::detect::result_t> detect_res = make_detect_result_from_box(box);

    core_trace(TAG_RECOG, "RECOGNITION_BEGIN");
    int64_t start_us = esp_timer_get_time();

    std::vector<dl::recognition::result_t> results =
        s_recognizer->recognize(img, detect_res);

    ESP_LOGI(TAG_RECOG,
             "[CORE-PROOF] RECOGNITION_END cpu=%d duration_us=%lld",
             xPortGetCoreID(),
             (long long)(esp_timer_get_time() - start_us));

    if (results.empty()) {
        strncpy(out_name, "unknown", out_name_len - 1);
        out_name[out_name_len - 1] = '\0';
        *out_score = 0.0f;

        ESP_LOGI(TAG_RECOG,
                 "Recognition result: unknown, no database match");

        return ESP_OK;
    }

    float similarity = results[0].similarity;
    *out_score = similarity;

    if (similarity < FACE_SIMILARITY_THRESHOLD) {
        strncpy(out_name, "unknown", out_name_len - 1);
        out_name[out_name_len - 1] = '\0';

        ESP_LOGI(TAG_RECOG,
                 "Face rejected: id=%d similarity=%.3f threshold=%.2f",
                 results[0].id,
                 similarity,
                 FACE_SIMILARITY_THRESHOLD);

        return ESP_OK;
    }

    /*
     * All features in the database currently belong to one person.
     */
    snprintf(out_name, out_name_len, "%s", FACE_PERSON_NAME);

    ESP_LOGI(TAG_RECOG,
             "Recognized id=%d as %s similarity=%.3f threshold=%.2f",
             results[0].id,
             FACE_PERSON_NAME,
             similarity,
             FACE_SIMILARITY_THRESHOLD);

    return ESP_OK;
}
