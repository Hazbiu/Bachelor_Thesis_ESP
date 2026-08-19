#pragma once

#include "config/app_features.h"

#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_FP32 || \
    APP_FACE_RECOG_BACKEND == APP_AI_BACKEND_TFLM_FP32

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "tensorflow/lite/c/common.h"

namespace tflite {
class AllOpsResolver;
class MicroInterpreter;
struct Model;
}

class TflmModelRunner {
public:
    TflmModelRunner();
    ~TflmModelRunner();

    esp_err_t init(const char *model_path, size_t tensor_arena_bytes);
    bool ready() const;

    TfLiteTensor *input(size_t index = 0);
    TfLiteTensor *output(size_t index = 0);
    size_t input_count() const;
    size_t output_count() const;

    esp_err_t invoke(int64_t *duration_us = nullptr);

    size_t model_size() const;
    uint64_t model_fingerprint() const;
    size_t arena_used_bytes() const;

private:
    TflmModelRunner(const TflmModelRunner &) = delete;
    TflmModelRunner &operator=(const TflmModelRunner &) = delete;

    uint8_t *model_data_;
    size_t model_size_;
    uint8_t *arena_raw_;
    uint8_t *arena_;
    size_t arena_size_;
    const tflite::Model *model_;
    tflite::AllOpsResolver *resolver_;
    tflite::MicroInterpreter *interpreter_;
    uint64_t model_fingerprint_;
};

float tflm_tensor_value_as_float(const TfLiteTensor *tensor, size_t index);
esp_err_t tflm_tensor_write_real(TfLiteTensor *tensor, size_t index, float value);
size_t tflm_tensor_element_count(const TfLiteTensor *tensor);

#endif
