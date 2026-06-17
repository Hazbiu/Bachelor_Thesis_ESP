#include "face_recognition_wrapper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <list>
#include <vector>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "human_face_recognition.hpp"
#include "dl_image_define.hpp"
#include "dl_detect_define.hpp"

#include "app_video.h"
#include "face_detect_wrapper.h"

static const char *TAG_RECOG = "face_recognition_wrapper";

static HumanFaceRecognizer *s_recognizer = nullptr;

#define FACE_DATABASE_PATH "/sdcard/face.db"
#define FACE_PERSON_NAME   "person_1"

/*
 * Recognition threshold.
 *
 * Increase this value if wrong people are recognized as the same person.
 * Decrease slightly if the correct person is often rejected.
 *
 * Good test values: 0.65, 0.70, 0.75
 */
#define FACE_SIMILARITY_THRESHOLD 0.70f

/*
 * SD-card image build settings.
 *
 * Python creates raw RGB888 files:
 * 320 * 240 * 3 = 230400 bytes
 */
#define FACE_DB_BUILD_IMAGE_WIDTH   320
#define FACE_DB_BUILD_IMAGE_HEIGHT  240
#define FACE_DB_BUILD_IMAGE_SIZE    (FACE_DB_BUILD_IMAGE_WIDTH * FACE_DB_BUILD_IMAGE_HEIGHT * 3)

#define FACE_DB_BUILD_MAX_IMAGES    50
#define FACE_DB_BUILD_MAX_BOXES     5
#define FACE_DB_BUILD_MIN_SCORE     0.60f

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
             "HumanFaceRecognizer initialized from %s, enrolled=%d",
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

/*
 * One-time database builder.
 *
 * This is NOT live camera enrollment.
 * This reads prepared RGB888 images from SD card and creates /sdcard/face.db.
 *
 * Expected SD-card structure:
 *
 * /sdcard/enroll/person_1/001.rgb
 * /sdcard/enroll/person_1/002.rgb
 * /sdcard/enroll/person_1/003.rgb
 */
extern "C" esp_err_t face_recognition_build_db_from_sd(void)
{
    ESP_LOGW(TAG_RECOG, "Building face database from SD card images");

    if (s_recognizer) {
        delete s_recognizer;
        s_recognizer = nullptr;
    }

    /*
     * Delete old database so we create a clean one.
     */
    unlink(FACE_DATABASE_PATH);

    s_recognizer = new HumanFaceRecognizer(FACE_DATABASE_PATH);

    if (!s_recognizer) {
        ESP_LOGE(TAG_RECOG, "Failed to create HumanFaceRecognizer");
        return ESP_FAIL;
    }

    uint8_t *img_buf = (uint8_t *)heap_caps_malloc(
        FACE_DB_BUILD_IMAGE_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (!img_buf) {
        img_buf = (uint8_t *)malloc(FACE_DB_BUILD_IMAGE_SIZE);
    }

    if (!img_buf) {
        ESP_LOGE(TAG_RECOG, "Failed to allocate image buffer");
        return ESP_ERR_NO_MEM;
    }

    int enrolled_images = 0;

    for (int image_index = 1; image_index <= FACE_DB_BUILD_MAX_IMAGES; image_index++) {
        char path[128];

        snprintf(
            path,
            sizeof(path),
            "/sdcard/enroll/person_1/%03d.rgb",
            image_index
        );

        FILE *f = fopen(path, "rb");

        if (!f) {
            continue;
        }

        ESP_LOGI(TAG_RECOG, "Reading %s", path);

        size_t bytes_read = fread(img_buf, 1, FACE_DB_BUILD_IMAGE_SIZE, f);
        fclose(f);

        if (bytes_read != FACE_DB_BUILD_IMAGE_SIZE) {
            ESP_LOGW(TAG_RECOG,
                     "Skipping %s: wrong file size, got=%d expected=%d",
                     path,
                     (int)bytes_read,
                     FACE_DB_BUILD_IMAGE_SIZE);
            continue;
        }

        face_box_t boxes[FACE_DB_BUILD_MAX_BOXES];

        int face_count = face_detect_run_rgb888(
            img_buf,
            FACE_DB_BUILD_IMAGE_WIDTH,
            FACE_DB_BUILD_IMAGE_HEIGHT,
            boxes,
            FACE_DB_BUILD_MAX_BOXES
        );

        ESP_LOGI(TAG_RECOG,
                 "%s: detected faces=%d",
                 path,
                 face_count);

        if (face_count <= 0) {
            ESP_LOGW(TAG_RECOG, "Skipping %s: no face detected", path);
            continue;
        }

        int best_index = 0;

        for (int i = 1; i < face_count; i++) {
            if (boxes[i].score > boxes[best_index].score) {
                best_index = i;
            }
        }

        ESP_LOGI(TAG_RECOG,
                 "%s: best face score=%.3f box=[%d,%d,%d,%d]",
                 path,
                 boxes[best_index].score,
                 boxes[best_index].x1,
                 boxes[best_index].y1,
                 boxes[best_index].x2,
                 boxes[best_index].y2);

        if (boxes[best_index].score < FACE_DB_BUILD_MIN_SCORE) {
            ESP_LOGW(TAG_RECOG,
                     "Skipping %s: face score %.3f below %.3f",
                     path,
                     boxes[best_index].score,
                     FACE_DB_BUILD_MIN_SCORE);
            continue;
        }

        dl::image::img_t img = {
            .data = img_buf,
            .width = FACE_DB_BUILD_IMAGE_WIDTH,
            .height = FACE_DB_BUILD_IMAGE_HEIGHT,
            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
        };

        std::list<dl::detect::result_t> detect_res =
            make_detect_result_from_box(&boxes[best_index]);

        /*
         * Internal feature creation.
         *
         * This uses Espressif's recognizer to create the embedding.
         * It is not live user enrollment from the camera.
         */
        esp_err_t ret = s_recognizer->enroll(img, detect_res);

        if (ret == ESP_OK) {
            enrolled_images++;

            ESP_LOGI(TAG_RECOG,
                     "Added embedding from %s, total=%d",
                     path,
                     s_recognizer->get_num_feats());
        } else {
            ESP_LOGE(TAG_RECOG,
                     "Failed to create embedding from %s: %s",
                     path,
                     esp_err_to_name(ret));
        }
    }

    free(img_buf);

    if (enrolled_images <= 0) {
        ESP_LOGE(TAG_RECOG, "No embeddings were created");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG_RECOG,
             "Face database build finished: embeddings=%d total=%d path=%s",
             enrolled_images,
             s_recognizer->get_num_feats(),
             FACE_DATABASE_PATH);

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
                 "Recognition skipped: no embeddings in %s",
                 FACE_DATABASE_PATH);

        return ESP_OK;
    }

    dl::image::img_t img = make_camera_image(
        camera_buf,
        width,
        height
    );

    std::list<dl::detect::result_t> detect_res = make_detect_result_from_box(box);

    std::vector<dl::recognition::result_t> results =
        s_recognizer->recognize(img, detect_res);

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
     * For now, all embeddings in the database belong to one person.
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