#include "config/app_features.h"

#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_FP32

#include "services/vision/backends/face_detector_backend.h"

#include <algorithm>
#include <math.h>
#include <string.h>
#include <vector>

#include "diagnostics/core_trace.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "services/vision/backends/tflm_model_runner.hpp"

static const char *TAG = "face_detect_tflm_fp32";
static TflmModelRunner s_runner;

struct letterbox_transform_t {
    float scale;
    float pad_x;
    float pad_y;
};

struct blaze_candidate_t {
    float ymin;
    float xmin;
    float ymax;
    float xmax;
    float keypoints[APP_TFLM_BLAZEFACE_NUM_KEYPOINTS * 2];
    float score;
};

static bool exact_detector_input(const TfLiteTensor *tensor)
{
    return tensor &&
           tensor->type == kTfLiteFloat32 &&
           tensor->dims &&
           tensor->dims->size == 4 &&
           tensor->dims->data[0] == 1 &&
           tensor->dims->data[1] == APP_TFLM_BLAZEFACE_INPUT_SIZE &&
           tensor->dims->data[2] == APP_TFLM_BLAZEFACE_INPUT_SIZE &&
           tensor->dims->data[3] == 3;
}

static bool exact_regressor_output(const TfLiteTensor *tensor)
{
    return tensor &&
           tensor->type == kTfLiteFloat32 &&
           tensor->dims &&
           tensor->dims->size == 3 &&
           tensor->dims->data[0] == 1 &&
           tensor->dims->data[1] == APP_TFLM_BLAZEFACE_NUM_ANCHORS &&
           tensor->dims->data[2] == APP_TFLM_BLAZEFACE_NUM_COORDS;
}

static bool exact_score_output(const TfLiteTensor *tensor)
{
    return tensor &&
           tensor->type == kTfLiteFloat32 &&
           tensor->dims &&
           tensor->dims->size == 3 &&
           tensor->dims->data[0] == 1 &&
           tensor->dims->data[1] == APP_TFLM_BLAZEFACE_NUM_ANCHORS &&
           tensor->dims->data[2] == 1;
}

static inline void read_rgb565_pixel(
    const uint8_t *buf,
    uint32_t width,
    uint32_t x,
    uint32_t y,
    float *r,
    float *g,
    float *b)
{
    const size_t index = (static_cast<size_t>(y) * width + x) * 2U;
    const uint16_t p = static_cast<uint16_t>(buf[index]) |
                       (static_cast<uint16_t>(buf[index + 1]) << 8);

    const uint8_t r5 = static_cast<uint8_t>((p >> 11) & 0x1F);
    const uint8_t g6 = static_cast<uint8_t>((p >> 5) & 0x3F);
    const uint8_t b5 = static_cast<uint8_t>(p & 0x1F);

    *r = static_cast<float>((r5 << 3) | (r5 >> 2));
    *g = static_cast<float>((g6 << 2) | (g6 >> 4));
    *b = static_cast<float>((b5 << 3) | (b5 >> 2));
}

static inline void read_rgb888_pixel(
    const uint8_t *buf,
    uint32_t width,
    uint32_t x,
    uint32_t y,
    float *r,
    float *g,
    float *b)
{
    const size_t index = (static_cast<size_t>(y) * width + x) * 3U;
    *r = static_cast<float>(buf[index + 0]);
    *g = static_cast<float>(buf[index + 1]);
    *b = static_cast<float>(buf[index + 2]);
}

static void sample_bilinear(
    const uint8_t *buf,
    uint32_t width,
    uint32_t height,
    float x,
    float y,
    bool rgb565,
    float *r,
    float *g,
    float *b)
{
    x = std::max(0.0f, std::min(x, static_cast<float>(width - 1U)));
    y = std::max(0.0f, std::min(y, static_cast<float>(height - 1U)));

    const uint32_t x0 = static_cast<uint32_t>(floorf(x));
    const uint32_t y0 = static_cast<uint32_t>(floorf(y));
    const uint32_t x1 = std::min<uint32_t>(x0 + 1U, width - 1U);
    const uint32_t y1 = std::min<uint32_t>(y0 + 1U, height - 1U);

    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);

    float r00, g00, b00;
    float r10, g10, b10;
    float r01, g01, b01;
    float r11, g11, b11;

    if (rgb565) {
        read_rgb565_pixel(buf, width, x0, y0, &r00, &g00, &b00);
        read_rgb565_pixel(buf, width, x1, y0, &r10, &g10, &b10);
        read_rgb565_pixel(buf, width, x0, y1, &r01, &g01, &b01);
        read_rgb565_pixel(buf, width, x1, y1, &r11, &g11, &b11);
    } else {
        read_rgb888_pixel(buf, width, x0, y0, &r00, &g00, &b00);
        read_rgb888_pixel(buf, width, x1, y0, &r10, &g10, &b10);
        read_rgb888_pixel(buf, width, x0, y1, &r01, &g01, &b01);
        read_rgb888_pixel(buf, width, x1, y1, &r11, &g11, &b11);
    }

    const float w00 = (1.0f - fx) * (1.0f - fy);
    const float w10 = fx * (1.0f - fy);
    const float w01 = (1.0f - fx) * fy;
    const float w11 = fx * fy;

    *r = r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11;
    *g = g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11;
    *b = b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11;
}

static inline float normalize_blazeface_pixel(float value)
{
    /* BlazeFace front model reference preprocessing: [0,255] -> [-1,1]. */
    return value / 127.5f - 1.0f;
}

static esp_err_t fill_blazeface_input(
    const uint8_t *buf,
    uint32_t width,
    uint32_t height,
    bool rgb565,
    letterbox_transform_t *transform)
{
    TfLiteTensor *input = s_runner.input(0);
    if (!buf || width == 0 || height == 0 || !transform ||
        !exact_detector_input(input)) {
        return ESP_ERR_INVALID_ARG;
    }

    const float dst =
        static_cast<float>(APP_TFLM_BLAZEFACE_INPUT_SIZE);
    transform->scale = std::min(
        dst / static_cast<float>(width),
        dst / static_cast<float>(height));

    const float scaled_w = static_cast<float>(width) * transform->scale;
    const float scaled_h = static_cast<float>(height) * transform->scale;
    transform->pad_x = (dst - scaled_w) * 0.5f;
    transform->pad_y = (dst - scaled_h) * 0.5f;

    float *out = input->data.f;
    size_t out_index = 0;

    for (int dy = 0; dy < APP_TFLM_BLAZEFACE_INPUT_SIZE; ++dy) {
        const float sy =
            (static_cast<float>(dy) + 0.5f - transform->pad_y) /
                transform->scale -
            0.5f;

        for (int dx = 0; dx < APP_TFLM_BLAZEFACE_INPUT_SIZE; ++dx) {
            const float sx =
                (static_cast<float>(dx) + 0.5f - transform->pad_x) /
                    transform->scale -
                0.5f;

            if (sx < -0.5f || sy < -0.5f ||
                sx > static_cast<float>(width) - 0.5f ||
                sy > static_cast<float>(height) - 0.5f) {
                /*
                 * Zero in normalized model space is neutral gray. MediaPipe's
                 * letterbox path also treats the padded tensor region as zero.
                 */
                out[out_index++] = 0.0f;
                out[out_index++] = 0.0f;
                out[out_index++] = 0.0f;
                continue;
            }

            float r, g, b;
            sample_bilinear(
                buf, width, height, sx, sy, rgb565, &r, &g, &b);

            out[out_index++] = normalize_blazeface_pixel(r);
            out[out_index++] = normalize_blazeface_pixel(g);
            out[out_index++] = normalize_blazeface_pixel(b);
        }
    }

    return ESP_OK;
}

/*
 * The 128x128 BlazeFace front model uses 896 fixed-size anchors.
 *
 * The model output order is:
 *   16x16 grid x 2 anchors = 512
 *    8x8  grid x 6 anchors = 384
 *
 * fixed_anchor_size=true, so each anchor width/height is 1.0 and only the
 * center is required by the decoder.
 */
static void blazeface_anchor_center(size_t index, float *cx, float *cy)
{
    if (index < 512U) {
        const size_t cell = index / 2U;
        const int x = static_cast<int>(cell % 16U);
        const int y = static_cast<int>(cell / 16U);
        *cx = (static_cast<float>(x) + 0.5f) / 16.0f;
        *cy = (static_cast<float>(y) + 0.5f) / 16.0f;
    } else {
        const size_t cell = (index - 512U) / 6U;
        const int x = static_cast<int>(cell % 8U);
        const int y = static_cast<int>(cell / 8U);
        *cx = (static_cast<float>(x) + 0.5f) / 8.0f;
        *cy = (static_cast<float>(y) + 0.5f) / 8.0f;
    }
}

static inline float sigmoid_clipped(float logit)
{
    logit = std::max(
        -APP_TFLM_BLAZEFACE_SCORE_CLIP,
        std::min(logit, APP_TFLM_BLAZEFACE_SCORE_CLIP));
    return 1.0f / (1.0f + expf(-logit));
}

static std::vector<blaze_candidate_t> decode_raw_predictions(void)
{
    std::vector<blaze_candidate_t> candidates;

    TfLiteTensor *regressors = s_runner.output(0);
    TfLiteTensor *scores = s_runner.output(1);
    if (!exact_regressor_output(regressors) || !exact_score_output(scores)) {
        return candidates;
    }

    candidates.reserve(64);

    const float *raw_boxes = regressors->data.f;
    const float *raw_scores = scores->data.f;

    for (size_t i = 0; i < APP_TFLM_BLAZEFACE_NUM_ANCHORS; ++i) {
        const float score = sigmoid_clipped(raw_scores[i]);
        if (score < APP_TFLM_BLAZEFACE_SCORE_THRESHOLD) {
            continue;
        }

        float anchor_x, anchor_y;
        blazeface_anchor_center(i, &anchor_x, &anchor_y);

        const float *raw =
            raw_boxes + i * APP_TFLM_BLAZEFACE_NUM_COORDS;

        const float x_center =
            raw[0] / APP_TFLM_BLAZEFACE_X_SCALE + anchor_x;
        const float y_center =
            raw[1] / APP_TFLM_BLAZEFACE_Y_SCALE + anchor_y;
        const float width =
            raw[2] / APP_TFLM_BLAZEFACE_W_SCALE;
        const float height =
            raw[3] / APP_TFLM_BLAZEFACE_H_SCALE;

        blaze_candidate_t candidate = {};
        candidate.ymin = y_center - height * 0.5f;
        candidate.xmin = x_center - width * 0.5f;
        candidate.ymax = y_center + height * 0.5f;
        candidate.xmax = x_center + width * 0.5f;
        candidate.score = score;

        for (int k = 0; k < APP_TFLM_BLAZEFACE_NUM_KEYPOINTS; ++k) {
            const int offset = 4 + k * 2;
            candidate.keypoints[k * 2 + 0] =
                raw[offset + 0] / APP_TFLM_BLAZEFACE_X_SCALE + anchor_x;
            candidate.keypoints[k * 2 + 1] =
                raw[offset + 1] / APP_TFLM_BLAZEFACE_Y_SCALE + anchor_y;
        }

        candidates.push_back(candidate);
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const blaze_candidate_t &a, const blaze_candidate_t &b) {
            return a.score > b.score;
        });

    return candidates;
}

static float candidate_iou(
    const blaze_candidate_t &a,
    const blaze_candidate_t &b)
{
    const float xmin = std::max(a.xmin, b.xmin);
    const float ymin = std::max(a.ymin, b.ymin);
    const float xmax = std::min(a.xmax, b.xmax);
    const float ymax = std::min(a.ymax, b.ymax);

    const float iw = std::max(0.0f, xmax - xmin);
    const float ih = std::max(0.0f, ymax - ymin);
    const float intersection = iw * ih;

    const float area_a =
        std::max(0.0f, a.xmax - a.xmin) *
        std::max(0.0f, a.ymax - a.ymin);
    const float area_b =
        std::max(0.0f, b.xmax - b.xmin) *
        std::max(0.0f, b.ymax - b.ymin);

    const float denom = area_a + area_b - intersection;
    return denom > 0.0f ? intersection / denom : 0.0f;
}

/*
 * BlazeFace uses weighted suppression rather than discarding every overlapping
 * box. Blend all detections that overlap the current highest-scoring box.
 */
static std::vector<blaze_candidate_t> weighted_nms(
    const std::vector<blaze_candidate_t> &input,
    int max_results)
{
    std::vector<blaze_candidate_t> remaining = input;
    std::vector<blaze_candidate_t> output;
    output.reserve(std::min<int>(max_results, remaining.size()));

    while (!remaining.empty() &&
           static_cast<int>(output.size()) < max_results) {
        const blaze_candidate_t seed = remaining.front();

        blaze_candidate_t blended = {};
        float weight_sum = 0.0f;
        float score_sum = 0.0f;
        int merged_count = 0;

        std::vector<blaze_candidate_t> next;
        next.reserve(remaining.size());

        for (const blaze_candidate_t &candidate : remaining) {
            if (candidate_iou(seed, candidate) >
                APP_TFLM_BLAZEFACE_NMS_IOU_THRESHOLD) {
                const float w = candidate.score;
                weight_sum += w;
                score_sum += candidate.score;
                merged_count++;

                blended.ymin += candidate.ymin * w;
                blended.xmin += candidate.xmin * w;
                blended.ymax += candidate.ymax * w;
                blended.xmax += candidate.xmax * w;

                for (int k = 0;
                     k < APP_TFLM_BLAZEFACE_NUM_KEYPOINTS * 2;
                     ++k) {
                    blended.keypoints[k] += candidate.keypoints[k] * w;
                }
            } else {
                next.push_back(candidate);
            }
        }

        if (weight_sum > 0.0f) {
            blended.ymin /= weight_sum;
            blended.xmin /= weight_sum;
            blended.ymax /= weight_sum;
            blended.xmax /= weight_sum;
            for (int k = 0;
                 k < APP_TFLM_BLAZEFACE_NUM_KEYPOINTS * 2;
                 ++k) {
                blended.keypoints[k] /= weight_sum;
            }
            blended.score =
                merged_count > 0 ? score_sum / merged_count : seed.score;
            output.push_back(blended);
        }

        remaining.swap(next);
    }

    return output;
}

static int model_x_to_source(
    float normalized_x,
    uint32_t source_width,
    const letterbox_transform_t &transform)
{
    const float model_x =
        normalized_x * APP_TFLM_BLAZEFACE_INPUT_SIZE;
    const float source_x =
        (model_x - transform.pad_x) / transform.scale;
    const int rounded = static_cast<int>(lrintf(source_x));

    return std::max(
        0,
        std::min(static_cast<int>(source_width) - 1, rounded));
}

static int model_y_to_source(
    float normalized_y,
    uint32_t source_height,
    const letterbox_transform_t &transform)
{
    const float model_y =
        normalized_y * APP_TFLM_BLAZEFACE_INPUT_SIZE;
    const float source_y =
        (model_y - transform.pad_y) / transform.scale;
    const int rounded = static_cast<int>(lrintf(source_y));

    return std::max(
        0,
        std::min(static_cast<int>(source_height) - 1, rounded));
}

static int write_face_boxes(
    const std::vector<blaze_candidate_t> &detections,
    uint32_t source_width,
    uint32_t source_height,
    const letterbox_transform_t &transform,
    face_box_t *boxes,
    int max_boxes)
{
    int count = 0;

    for (const blaze_candidate_t &detection : detections) {
        if (count >= max_boxes) {
            break;
        }

        face_box_t box = {};
        box.x1 =
            model_x_to_source(detection.xmin, source_width, transform);
        box.y1 =
            model_y_to_source(detection.ymin, source_height, transform);
        box.x2 =
            model_x_to_source(detection.xmax, source_width, transform);
        box.y2 =
            model_y_to_source(detection.ymax, source_height, transform);
        box.score = detection.score;

        if (box.x2 <= box.x1 || box.y2 <= box.y1) {
            continue;
        }

        /*
         * face_box_t currently stores 10 integers. BlazeFace predicts six
         * keypoints (12 numbers), so retain the first five:
         *   eye, eye, nose, mouth, one ear tragion.
         * The first two eye points are sufficient for MobileFaceNet alignment.
         */
        box.keypoint_count = 10;
        for (int k = 0; k < 5; ++k) {
            box.keypoints[k * 2 + 0] = model_x_to_source(
                detection.keypoints[k * 2 + 0],
                source_width,
                transform);
            box.keypoints[k * 2 + 1] = model_y_to_source(
                detection.keypoints[k * 2 + 1],
                source_height,
                transform);
        }

        boxes[count++] = box;
    }

    return count;
}

static int run_common(
    uint8_t *buf,
    uint32_t width,
    uint32_t height,
    face_box_t *boxes,
    int max_boxes,
    bool rgb565)
{
    if (!s_runner.ready() || !buf || !boxes ||
        width == 0 || height == 0 || max_boxes <= 0) {
        return 0;
    }

    const int64_t total_start_us = esp_timer_get_time();

    letterbox_transform_t transform = {};
    if (fill_blazeface_input(
            buf, width, height, rgb565, &transform) != ESP_OK) {
        return 0;
    }

    const int64_t after_preprocess_us = esp_timer_get_time();

    core_trace(TAG, "DETECT_BEGIN");
    int64_t invoke_us = 0;
    const esp_err_t invoke_ret = s_runner.invoke(&invoke_us);
    if (invoke_ret != ESP_OK) {
        ESP_LOGE(TAG, "BlazeFace Invoke failed: %s",
                 esp_err_to_name(invoke_ret));
        return 0;
    }

    std::vector<blaze_candidate_t> decoded = decode_raw_predictions();
    std::vector<blaze_candidate_t> filtered =
        weighted_nms(decoded, max_boxes);

    const int count = write_face_boxes(
        filtered, width, height, transform, boxes, max_boxes);

    const int64_t end_us = esp_timer_get_time();

    ESP_LOGI(
        TAG,
        "[AI-BENCH] backend=TFLM-FP32 model=BlazeFace "
        "stage=DETECT cpu=%d preprocess_us=%lld invoke_us=%lld "
        "postprocess_us=%lld total_us=%lld faces=%d candidates=%u",
        xPortGetCoreID(),
        (long long)(after_preprocess_us - total_start_us),
        (long long)invoke_us,
        (long long)(end_us - after_preprocess_us - invoke_us),
        (long long)(end_us - total_start_us),
        count,
        (unsigned)decoded.size());

    return count;
}

extern "C" esp_err_t tflm_fp32_face_detect_init(void)
{
    const esp_err_t ret = s_runner.init(
        APP_TFLM_FACE_DETECT_MODEL_PATH,
        APP_TFLM_DETECT_TENSOR_ARENA_BYTES);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "BlazeFace model initialization failed: path=%s error=%s",
            APP_TFLM_FACE_DETECT_MODEL_PATH,
            esp_err_to_name(ret));
        return ret;
    }

    TfLiteTensor *input = s_runner.input(0);
    TfLiteTensor *regressors = s_runner.output(0);
    TfLiteTensor *scores = s_runner.output(1);

    if (!exact_detector_input(input) ||
        !exact_regressor_output(regressors) ||
        !exact_score_output(scores)) {
        ESP_LOGE(
            TAG,
            "Wrong BlazeFace tensor contract. Required: "
            "input=[1,128,128,3] F32, out0=[1,896,16] F32, "
            "out1=[1,896,1] F32");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(
        TAG,
        "TFLM-FP32 BlazeFace ready: model=%s arena_used=%u",
        APP_TFLM_FACE_DETECT_MODEL_PATH,
        (unsigned)s_runner.arena_used_bytes());

    return ESP_OK;
}

extern "C" int tflm_fp32_face_detect_run_rgb565(
    uint8_t *buf,
    uint32_t width,
    uint32_t height,
    face_box_t *boxes,
    int max_boxes)
{
    return run_common(buf, width, height, boxes, max_boxes, true);
}

extern "C" int tflm_fp32_face_detect_run_rgb888(
    uint8_t *buf,
    uint32_t width,
    uint32_t height,
    face_box_t *boxes,
    int max_boxes)
{
    return run_common(buf, width, height, boxes, max_boxes, false);
}

#else

#include "services/vision/backends/face_detector_backend.h"

extern "C" esp_err_t tflm_fp32_face_detect_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

extern "C" int tflm_fp32_face_detect_run_rgb565(
    uint8_t *, uint32_t, uint32_t, face_box_t *, int)
{
    return 0;
}

extern "C" int tflm_fp32_face_detect_run_rgb888(
    uint8_t *, uint32_t, uint32_t, face_box_t *, int)
{
    return 0;
}

#endif
