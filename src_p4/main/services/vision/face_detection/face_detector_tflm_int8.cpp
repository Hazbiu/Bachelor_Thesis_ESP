#include "config/app_features.h"

#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_INT8

#include "services/vision/backends/face_detector_backend.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "diagnostics/core_trace.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "services/vision/backends/tflm_model_runner.hpp"

static const char *TAG = "face_detect_tflm_int8";
static TflmModelRunner s_runner;
static uint32_t s_live_detector_diag_calls = 0;

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
           tensor->type == kTfLiteInt8 &&
           tensor->dims &&
           tensor->dims->size == 4 &&
           tensor->dims->data[0] == 1 &&
           tensor->dims->data[1] == APP_TFLM_BLAZEFACE_INPUT_SIZE &&
           tensor->dims->data[2] == APP_TFLM_BLAZEFACE_INPUT_SIZE &&
           tensor->dims->data[3] == 3 &&
           tensor->params.scale > 0.0f;
}

static bool exact_regressor_output(const TfLiteTensor *tensor)
{
    return tensor &&
           tensor->type == kTfLiteInt8 &&
           tensor->dims &&
           tensor->dims->size == 3 &&
           tensor->dims->data[0] == 1 &&
           tensor->dims->data[1] == APP_TFLM_BLAZEFACE_NUM_ANCHORS &&
           tensor->dims->data[2] == APP_TFLM_BLAZEFACE_NUM_COORDS &&
           tensor->params.scale > 0.0f;
}

static bool exact_score_output(const TfLiteTensor *tensor)
{
    return tensor &&
           tensor->type == kTfLiteInt8 &&
           tensor->dims &&
           tensor->dims->size == 3 &&
           tensor->dims->data[0] == 1 &&
           tensor->dims->data[1] == APP_TFLM_BLAZEFACE_NUM_ANCHORS &&
           tensor->dims->data[2] == 1 &&
           tensor->params.scale > 0.0f;
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

    if (input->params.scale <= 0.0f) {
        return ESP_ERR_INVALID_STATE;
    }

    int8_t *out = input->data.int8;
    size_t out_index = 0;

    const float inv_scale = 1.0f / input->params.scale;
    const int zero_point = input->params.zero_point;

    auto write_real = [&](float value) {
        int q = static_cast<int>(lrintf(value * inv_scale)) + zero_point;
        q = std::max(-128, std::min(127, q));
        out[out_index++] = static_cast<int8_t>(q);
    };

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
                 * Real value 0.0 is the same neutral-gray padding used by the
                 * FP32 backend. Quantize it with the model's own scale/zero
                 * point instead of assuming a particular integer value.
                 */
                write_real(0.0f);
                write_real(0.0f);
                write_real(0.0f);
                continue;
            }

            float r, g, b;
            sample_bilinear(
                buf, width, height, sx, sy, rgb565, &r, &g, &b);

            write_real(normalize_blazeface_pixel(r));
            write_real(normalize_blazeface_pixel(g));
            write_real(normalize_blazeface_pixel(b));
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

/*
 * INT8 score-threshold mapping
 * ----------------------------
 *
 * The FP32 detector keeps APP_TFLM_BLAZEFACE_SCORE_THRESHOLD (0.75).
 * This INT8 backend uses its own candidate threshold because its score
 * tensor has a very coarse output grid.
 *
 * The INT8 classifier output has a finite quantization grid.  With the
 * generated FDET8.TFL observed during conversion:
 *
 *     scale      ~= 0.75656235
 *     zero_point = 122
 *
 * the old 0.75 probability target fell between representable INT8 values,
 * which is BETWEEN two representable INT8 logits:
 *
 *     q=123 -> logit ~= 0.7566 -> sigmoid ~= 0.6806
 *     q=124 -> logit ~= 1.5131 -> sigmoid ~= 0.8195
 *
 * The old implementation dequantized q and then compared against 0.75.
 * That accidentally rounded the threshold UP to q>=124, making the
 * effective detector threshold about 0.8195.  That is why all 13 known
 * enrollment faces and all live frames were rejected after quantization.
 *
 * The live detector now uses the explicit INT8 candidate threshold 0.50,
 * which maps exactly to q=122. This is a candidate-generation threshold;
 * MobileFaceNet recognition remains the second-stage identity verifier.
 *
 * This changes ONLY the INT8 detector post-processing.  ESP-DL and TFLM-FP32
 * keep their existing thresholds and behavior.
 */
static int quantized_score_threshold_q(const TfLiteTensor *scores)
{
    if (!scores || scores->params.scale <= 0.0f) {
        return 127;
    }

    const float probability = std::max(
        1.0e-6f,
        std::min(
            APP_TFLM_INT8_BLAZEFACE_SCORE_THRESHOLD,
            1.0f - 1.0e-6f));

    const float threshold_logit =
        logf(probability / (1.0f - probability));

    int q = static_cast<int>(
        lrintf(threshold_logit / scores->params.scale)) +
        scores->params.zero_point;

    return std::max(-128, std::min(127, q));
}

static float quantized_score_threshold_effective(
    const TfLiteTensor *scores,
    int threshold_q)
{
    if (!scores || scores->params.scale <= 0.0f) {
        return APP_TFLM_INT8_BLAZEFACE_SCORE_THRESHOLD;
    }

    const float represented_logit =
        (threshold_q - scores->params.zero_point) *
        scores->params.scale;

    return sigmoid_clipped(represented_logit);
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

    if (regressors->params.scale <= 0.0f ||
        scores->params.scale <= 0.0f) {
        return candidates;
    }

    const int8_t *raw_boxes = regressors->data.int8;
    const int8_t *raw_scores = scores->data.int8;
    const float box_scale = regressors->params.scale;
    const int box_zero = regressors->params.zero_point;
    const float score_scale = scores->params.scale;
    const int score_zero = scores->params.zero_point;
    const int score_threshold_q = quantized_score_threshold_q(scores);

    auto box_value = [&](size_t index) {
        return (static_cast<int>(raw_boxes[index]) - box_zero) * box_scale;
    };

    for (size_t i = 0; i < APP_TFLM_BLAZEFACE_NUM_ANCHORS; ++i) {
        const int raw_score_q = static_cast<int>(raw_scores[i]);

        /*
         * Compare on the INT8 grid.  Do NOT dequantize and compare against
         * 0.75 directly: because the classifier output is coarse, doing that
         * silently turned 0.75 into the stricter q>=124 (~0.8195).
         */
        if (raw_score_q < score_threshold_q) {
            continue;
        }

        const float raw_score =
            (raw_score_q - score_zero) * score_scale;
        const float score = sigmoid_clipped(raw_score);

        float anchor_x, anchor_y;
        blazeface_anchor_center(i, &anchor_x, &anchor_y);

        const size_t base = i * APP_TFLM_BLAZEFACE_NUM_COORDS;

        const float x_center =
            box_value(base + 0) / APP_TFLM_BLAZEFACE_X_SCALE + anchor_x;
        const float y_center =
            box_value(base + 1) / APP_TFLM_BLAZEFACE_Y_SCALE + anchor_y;
        const float width =
            box_value(base + 2) / APP_TFLM_BLAZEFACE_W_SCALE;
        const float height =
            box_value(base + 3) / APP_TFLM_BLAZEFACE_H_SCALE;

        blaze_candidate_t candidate = {};
        candidate.ymin = y_center - height * 0.5f;
        candidate.xmin = x_center - width * 0.5f;
        candidate.ymax = y_center + height * 0.5f;
        candidate.xmax = x_center + width * 0.5f;
        candidate.score = score;

        for (int k = 0; k < APP_TFLM_BLAZEFACE_NUM_KEYPOINTS; ++k) {
            const size_t offset = base + 4U + static_cast<size_t>(k) * 2U;
            candidate.keypoints[k * 2 + 0] =
                box_value(offset + 0) / APP_TFLM_BLAZEFACE_X_SCALE + anchor_x;
            candidate.keypoints[k * 2 + 1] =
                box_value(offset + 1) / APP_TFLM_BLAZEFACE_Y_SCALE + anchor_y;
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

/*
 * Door-access live view policy for the INT8 detector.
 *
 * At q=122 (0.50), the coarse detector score grid can produce several
 * unrelated candidates with exactly the same confidence.  Showing all of
 * those as "faces" creates multiple boxes even when one person is present.
 *
 * For LIVE RGB565 frames only, keep one candidate:
 *   1. highest detector score,
 *   2. if tied, largest positive box area,
 *   3. if still tied, closest to the image center.
 *
 * Enrollment uses RGB888 and is intentionally left unchanged.
 */
static blaze_candidate_t select_best_live_candidate(
    const std::vector<blaze_candidate_t> &detections)
{
    blaze_candidate_t best = detections.front();

    auto area_of = [](const blaze_candidate_t &d) {
        const float w = std::max(0.0f, d.xmax - d.xmin);
        const float h = std::max(0.0f, d.ymax - d.ymin);
        return w * h;
    };

    auto center_distance_sq = [](const blaze_candidate_t &d) {
        const float cx = (d.xmin + d.xmax) * 0.5f;
        const float cy = (d.ymin + d.ymax) * 0.5f;
        const float dx = cx - 0.5f;
        const float dy = cy - 0.5f;
        return dx * dx + dy * dy;
    };

    for (size_t i = 1; i < detections.size(); ++i) {
        const blaze_candidate_t &candidate = detections[i];

        if (candidate.score > best.score + 1.0e-6f) {
            best = candidate;
            continue;
        }

        if (fabsf(candidate.score - best.score) <= 1.0e-6f) {
            const float candidate_area = area_of(candidate);
            const float best_area = area_of(best);

            if (candidate_area > best_area + 1.0e-6f) {
                best = candidate;
                continue;
            }

            if (fabsf(candidate_area - best_area) <= 1.0e-6f &&
                center_distance_sq(candidate) < center_distance_sq(best)) {
                best = candidate;
            }
        }
    }

    return best;
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

    /*
     * Diagnostic score audit for the quantized detector.  This is deliberately
     * computed from the real INT8 output tensor after Invoke(), using the same
     * scale/zero-point conversion as decode_raw_predictions().  It tells us
     * immediately whether a zero-face result is caused by the score threshold
     * or by invalid decoded geometry.
     */
    TfLiteTensor *score_tensor = s_runner.output(1);
    int max_score_q = -128;
    float max_score = 0.0f;
    if (exact_score_output(score_tensor)) {
        const int8_t *score_data = score_tensor->data.int8;
        for (size_t i = 0; i < APP_TFLM_BLAZEFACE_NUM_ANCHORS; ++i) {
            const int q = static_cast<int>(score_data[i]);
            if (q > max_score_q) {
                max_score_q = q;
            }
        }
        const float max_logit =
            (max_score_q - score_tensor->params.zero_point) *
            score_tensor->params.scale;
        max_score = sigmoid_clipped(max_logit);
    }

    std::vector<blaze_candidate_t> decoded = decode_raw_predictions();
    std::vector<blaze_candidate_t> filtered =
        weighted_nms(decoded, max_boxes);

    const unsigned filtered_before_live_limit =
        static_cast<unsigned>(filtered.size());

    if (rgb565 && filtered.size() > 1U) {
        const blaze_candidate_t best =
            select_best_live_candidate(filtered);
        filtered.clear();
        filtered.push_back(best);
    }

    const int count = write_face_boxes(
        filtered, width, height, transform, boxes, max_boxes);

    /*
     * The normal ESP_LOG tag can be filtered by runtime logging settings.
     * For live RGB565 frames, print one compact line every eighth invocation
     * (and every positive detection) so detector behavior is always observable.
     */
    if (rgb565) {
        s_live_detector_diag_calls++;
        if (count > 0 || (s_live_detector_diag_calls % 8U) == 1U) {
            printf(
                "[INT8-LIVE-DETECT] call=%u max_q=%d max_prob=%.6f "
                "threshold_q=%d threshold_prob=%.6f candidates=%u "
                "nms=%u live_faces=%d\n",
                (unsigned)s_live_detector_diag_calls,
                max_score_q,
                (double)max_score,
                quantized_score_threshold_q(score_tensor),
                (double)quantized_score_threshold_effective(
                    score_tensor,
                    quantized_score_threshold_q(score_tensor)),
                (unsigned)decoded.size(),
                filtered_before_live_limit,
                count);
        }
    }

    const int64_t end_us = esp_timer_get_time();

    ESP_LOGI(
        TAG,
        "[AI-BENCH] backend=TFLM-INT8+ESP-NN model=BlazeFace "
        "stage=DETECT cpu=%d preprocess_us=%lld invoke_us=%lld "
        "postprocess_us=%lld total_us=%lld faces=%d candidates=%u "
        "max_score=%.6f max_score_q=%d threshold_nominal=%.3f "
        "threshold_q=%d threshold_effective=%.6f",
        xPortGetCoreID(),
        (long long)(after_preprocess_us - total_start_us),
        (long long)invoke_us,
        (long long)(end_us - after_preprocess_us - invoke_us),
        (long long)(end_us - total_start_us),
        count,
        (unsigned)decoded.size(),
        (double)max_score,
        max_score_q,
        (double)APP_TFLM_INT8_BLAZEFACE_SCORE_THRESHOLD,
        quantized_score_threshold_q(score_tensor),
        (double)quantized_score_threshold_effective(
            score_tensor,
            quantized_score_threshold_q(score_tensor)));

    return count;
}

extern "C" esp_err_t tflm_int8_face_detect_init(void)
{
    const esp_err_t ret = s_runner.init(
        APP_TFLM_INT8_FACE_DETECT_MODEL_PATH,
        APP_TFLM_DETECT_TENSOR_ARENA_BYTES);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "BlazeFace model initialization failed: path=%s error=%s",
            APP_TFLM_INT8_FACE_DETECT_MODEL_PATH,
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
            "input=[1,128,128,3] INT8, out0=[1,896,16] INT8, "
            "out1=[1,896,1] INT8");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const int threshold_q = quantized_score_threshold_q(scores);
    const float threshold_effective =
        quantized_score_threshold_effective(scores, threshold_q);

    ESP_LOGI(
        TAG,
        "TFLM-INT8+ESP-NN BlazeFace ready: model=%s arena_used=%u "
        "input_scale=%.9f input_zero=%d score_scale=%.9f score_zero=%d "
        "threshold_nominal=%.3f threshold_q=%d threshold_effective=%.6f",
        APP_TFLM_INT8_FACE_DETECT_MODEL_PATH,
        (unsigned)s_runner.arena_used_bytes(),
        (double)input->params.scale,
        input->params.zero_point,
        (double)scores->params.scale,
        scores->params.zero_point,
        (double)APP_TFLM_INT8_BLAZEFACE_SCORE_THRESHOLD,
        threshold_q,
        (double)threshold_effective);

    /*
     * Always print one compact marker even if ESP-IDF tag filtering changes.
     * This makes the INT8 threshold mapping visible in serial benchmark logs.
     */
    printf(
        "[INT8-DETECT-FIX] nominal=%.3f score_scale=%.9f score_zero=%d "
        "threshold_q=%d threshold_effective=%.6f\n",
        (double)APP_TFLM_INT8_BLAZEFACE_SCORE_THRESHOLD,
        (double)scores->params.scale,
        (int)scores->params.zero_point,
        threshold_q,
        (double)threshold_effective);

    return ESP_OK;
}

extern "C" int tflm_int8_face_detect_run_rgb565(
    uint8_t *buf,
    uint32_t width,
    uint32_t height,
    face_box_t *boxes,
    int max_boxes)
{
    return run_common(buf, width, height, boxes, max_boxes, true);
}

extern "C" int tflm_int8_face_detect_run_rgb888(
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

extern "C" esp_err_t tflm_int8_face_detect_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

extern "C" int tflm_int8_face_detect_run_rgb565(
    uint8_t *, uint32_t, uint32_t, face_box_t *, int)
{
    return 0;
}

extern "C" int tflm_int8_face_detect_run_rgb888(
    uint8_t *, uint32_t, uint32_t, face_box_t *, int)
{
    return 0;
}

#endif
