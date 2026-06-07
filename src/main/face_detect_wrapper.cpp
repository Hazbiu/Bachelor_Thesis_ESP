#include "face_detect_wrapper.h"

#include "esp_log.h"
#include "human_face_detect.hpp"
#include "dl_image_define.hpp"

static const char *TAG_FACE = "face_detect_wrapper";

static HumanFaceDetect *s_face_detect = nullptr;

extern "C" esp_err_t face_detect_init(void)
{
    if (s_face_detect) {
        return ESP_OK;
    }

    s_face_detect = new HumanFaceDetect();
    if (!s_face_detect) {
        ESP_LOGE(TAG_FACE, "Failed to create HumanFaceDetect");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG_FACE, "HumanFaceDetect initialized");
    return ESP_OK;
}

static int face_detect_run_common(
    uint8_t *buf,
    uint32_t width,
    uint32_t height,
    face_box_t *boxes,
    int max_boxes,
    dl::image::pix_type_t pix_type)
{
    if (!s_face_detect || !buf || !boxes || max_boxes <= 0) {
        return 0;
    }

    dl::image::img_t img = {
        .data = buf,
        .width = (uint16_t)width,
        .height = (uint16_t)height,
        .pix_type = pix_type,
    };

    auto &results = s_face_detect->run(img);

    int count = 0;

    for (const auto &res : results) {
        if (count >= max_boxes) {
            break;
        }

        boxes[count].x1 = res.box[0];
        boxes[count].y1 = res.box[1];
        boxes[count].x2 = res.box[2];
        boxes[count].y2 = res.box[3];
        boxes[count].score = res.score;

        count++;
    }

    return count;
}

extern "C" int face_detect_run_rgb565(
    uint8_t *buf,
    uint32_t width,
    uint32_t height,
    face_box_t *boxes,
    int max_boxes)
{
    return face_detect_run_common(
        buf,
        width,
        height,
        boxes,
        max_boxes,
        dl::image::DL_IMAGE_PIX_TYPE_RGB565LE
    );
}

extern "C" int face_detect_run_rgb888(
    uint8_t *buf,
    uint32_t width,
    uint32_t height,
    face_box_t *boxes,
    int max_boxes)
{
    return face_detect_run_common(
        buf,
        width,
        height,
        boxes,
        max_boxes,
        dl::image::DL_IMAGE_PIX_TYPE_RGB888
    );
}
