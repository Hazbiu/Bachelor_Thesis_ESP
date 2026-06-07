#include "face_recognition_wrapper.h"

#include <inttypes.h>
#include <string.h>
#include <list>
#include <vector>

#include "esp_log.h"
#include "human_face_recognition.hpp"
#include "dl_image_define.hpp"
#include "dl_detect_define.hpp"

static const char *TAG_RECOG = "face_recognition_wrapper";

static HumanFaceRecognizer *s_recognizer = nullptr;

static std::list<dl::detect::result_t> make_detect_result_from_box(const face_box_t *box)
{
    std::list<dl::detect::result_t> detect_res;

    dl::detect::result_t r;
    r.category = 0;
    r.score = box->score;
    r.box = {box->x1, box->y1, box->x2, box->y2};

    /*
     * Temporary approximate 5-point landmarks.
     * HumanFaceRecognizer expects landmarks for alignment.
     * Later we should replace these with real keypoints from HumanFaceDetect.
     */
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

extern "C" esp_err_t face_recognition_init(void)
{
    if (s_recognizer) {
        return ESP_OK;
    }

    /*
     * Database path:
     * Start with a simple path. If this fails at runtime, we will switch to
     * a mounted filesystem path such as /spiffs/face.db or /littlefs/face.db.
     */
    s_recognizer = new HumanFaceRecognizer("/spiffs/face.db");

    if (!s_recognizer) {
        ESP_LOGE(TAG_RECOG, "Failed to create HumanFaceRecognizer");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG_RECOG, "HumanFaceRecognizer initialized, enrolled=%d",
             s_recognizer->get_num_feats());

    return ESP_OK;
}

extern "C" esp_err_t face_recognition_enroll(
    uint8_t *camera_buf,
    uint32_t width,
    uint32_t height,
    const face_box_t *box,
    const char *name)
{
    if (!s_recognizer || !camera_buf || !box || !name) {
        return ESP_ERR_INVALID_ARG;
    }

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

    std::list<dl::detect::result_t> detect_res = make_detect_result_from_box(box);

    esp_err_t ret = s_recognizer->enroll(img, detect_res);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG_RECOG,
                 "Enrolled face name=%s, total=%d",
                 name,
                 s_recognizer->get_num_feats());
    } else {
        ESP_LOGE(TAG_RECOG, "Enroll failed: %s", esp_err_to_name(ret));
    }

    return ret;
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
    if (!s_recognizer || !camera_buf || !box || !out_name || out_name_len <= 0 || !out_score) {
        return ESP_ERR_INVALID_ARG;
    }

    out_name[0] = '\0';
    *out_score = 0.0f;

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

    std::list<dl::detect::result_t> detect_res = make_detect_result_from_box(box);

    std::vector<dl::recognition::result_t> results = s_recognizer->recognize(img, detect_res);

    if (results.empty()) {
        strncpy(out_name, "unknown", out_name_len - 1);
        out_name[out_name_len - 1] = '\0';
        *out_score = 0.0f;
        return ESP_OK;
    }

    /*
     * We do not yet know the exact fields of result_t in your version.
     * If this part fails to compile, paste the compiler error and the
     * dl_recognition_define.hpp content.
     */
    snprintf(out_name, out_name_len, "id_%d", results[0].id);
    *out_score = results[0].similarity;

    ESP_LOGI(TAG_RECOG,
             "Recognized id=%d similarity=%.3f",
             results[0].id,
             results[0].similarity);

    return ESP_OK;
}