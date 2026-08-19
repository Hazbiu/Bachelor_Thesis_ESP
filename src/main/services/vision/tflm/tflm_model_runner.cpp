#include "config/app_features.h"

#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_FP32 || \
    APP_FACE_RECOG_BACKEND == APP_AI_BACKEND_TFLM_FP32

#include "services/vision/backends/tflm_model_runner.hpp"

#include <math.h>
#include <new>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "tflm_model_runner";

static uint64_t fnv1a64(const void *data, size_t size)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static esp_err_t register_model_ops(AppTflmOpResolver *resolver)
{
    if (!resolver) {
        return ESP_ERR_INVALID_ARG;
    }

#define ADD_TFLM_OP(call) do { \
    if ((call) != kTfLiteOk) { \
        ESP_LOGE(TAG, "Failed to register TFLM op: %s", #call); \
        return ESP_FAIL; \
    } \
} while (0)

    /*
     * Broad resolver for the BlazeFace FP32 + MobileFaceNet FP32 experiment.
     * This replaces the removed AllOpsResolver API in current Espressif TFLM.
     * If AllocateTensors later reports one missing builtin opcode, add only
     * that specific operator here.
     */
    ADD_TFLM_OP(resolver->AddConv2D());
    ADD_TFLM_OP(resolver->AddDepthwiseConv2D());
    ADD_TFLM_OP(resolver->AddAdd());
    ADD_TFLM_OP(resolver->AddMul());
    ADD_TFLM_OP(resolver->AddSub());
    ADD_TFLM_OP(resolver->AddAbs());
    ADD_TFLM_OP(resolver->AddRelu());
    ADD_TFLM_OP(resolver->AddRelu6());
    ADD_TFLM_OP(resolver->AddPrelu());
    ADD_TFLM_OP(resolver->AddLeakyRelu());
    ADD_TFLM_OP(resolver->AddMaxPool2D());
    ADD_TFLM_OP(resolver->AddAveragePool2D());
    ADD_TFLM_OP(resolver->AddFullyConnected());
    ADD_TFLM_OP(resolver->AddReshape());
    ADD_TFLM_OP(resolver->AddMean());
    ADD_TFLM_OP(resolver->AddPad());
    ADD_TFLM_OP(resolver->AddPadV2());
    ADD_TFLM_OP(resolver->AddConcatenation());
    ADD_TFLM_OP(resolver->AddResizeBilinear());
    ADD_TFLM_OP(resolver->AddResizeNearestNeighbor());
    ADD_TFLM_OP(resolver->AddLogistic());
    ADD_TFLM_OP(resolver->AddSoftmax());
    ADD_TFLM_OP(resolver->AddL2Normalization());
    ADD_TFLM_OP(resolver->AddSqueeze());
    ADD_TFLM_OP(resolver->AddTranspose());
    ADD_TFLM_OP(resolver->AddStridedSlice());
    ADD_TFLM_OP(resolver->AddShape());
    ADD_TFLM_OP(resolver->AddPack());
    ADD_TFLM_OP(resolver->AddSlice());
    ADD_TFLM_OP(resolver->AddDequantize());
    ADD_TFLM_OP(resolver->AddQuantize());
    ADD_TFLM_OP(resolver->AddExpandDims());
    ADD_TFLM_OP(resolver->AddBatchToSpaceNd());

#undef ADD_TFLM_OP
    return ESP_OK;
}

TflmModelRunner::TflmModelRunner()
    : model_data_(nullptr),
      model_size_(0),
      arena_raw_(nullptr),
      arena_(nullptr),
      arena_size_(0),
      model_(nullptr),
      resolver_(nullptr),
      interpreter_(nullptr),
      model_fingerprint_(0)
{
}

TflmModelRunner::~TflmModelRunner()
{
    delete interpreter_;
    delete resolver_;
    if (arena_raw_) {
        heap_caps_free(arena_raw_);
    }
    if (model_data_) {
        heap_caps_free(model_data_);
    }
}

esp_err_t TflmModelRunner::init(const char *model_path, size_t tensor_arena_bytes)
{
    if (!model_path || tensor_arena_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ready()) {
        return ESP_OK;
    }

    FILE *file = fopen(model_path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "TFLM model not found: %s", model_path);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    const long file_size = ftell(file);
    if (file_size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    model_size_ = static_cast<size_t>(file_size);
    model_data_ = static_cast<uint8_t *>(
        heap_caps_malloc(model_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!model_data_) {
        fclose(file);
        ESP_LOGE(TAG, "Could not allocate %u bytes for model", (unsigned)model_size_);
        return ESP_ERR_NO_MEM;
    }

    const size_t got = fread(model_data_, 1, model_size_, file);
    fclose(file);
    if (got != model_size_) {
        ESP_LOGE(TAG, "Short read loading model: got=%u expected=%u",
                 (unsigned)got, (unsigned)model_size_);
        return ESP_FAIL;
    }

    model_fingerprint_ = fnv1a64(model_data_, model_size_);
    model_ = tflite::GetModel(model_data_);
    if (!model_ || model_->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(
            TAG,
            "TFLM schema mismatch: model=%d runtime=%d",
            model_ ? model_->version() : -1,
            TFLITE_SCHEMA_VERSION);
        return ESP_ERR_NOT_SUPPORTED;
    }

    arena_size_ = tensor_arena_bytes;
    arena_raw_ = static_cast<uint8_t *>(
        heap_caps_malloc(arena_size_ + 16U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!arena_raw_) {
        ESP_LOGE(TAG, "Could not allocate %u-byte tensor arena", (unsigned)arena_size_);
        return ESP_ERR_NO_MEM;
    }

    const uintptr_t aligned =
        (reinterpret_cast<uintptr_t>(arena_raw_) + 15U) & ~uintptr_t(15U);
    arena_ = reinterpret_cast<uint8_t *>(aligned);

    resolver_ = new (std::nothrow) AppTflmOpResolver();
    if (!resolver_) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t resolver_ret = register_model_ops(resolver_);
    if (resolver_ret != ESP_OK) {
        return resolver_ret;
    }

    interpreter_ = new (std::nothrow) tflite::MicroInterpreter(
        model_, *resolver_, arena_, arena_size_);
    if (!interpreter_) {
        return ESP_ERR_NO_MEM;
    }

    if (interpreter_->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(
            TAG,
            "AllocateTensors failed for %s; inspect the preceding TFLM log for a missing op or increase the arena",
            model_path);
        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "TFLM model ready: path=%s bytes=%u arena=%u used=%u inputs=%u outputs=%u hash=%016llx",
        model_path,
        (unsigned)model_size_,
        (unsigned)arena_size_,
        (unsigned)interpreter_->arena_used_bytes(),
        (unsigned)interpreter_->inputs_size(),
        (unsigned)interpreter_->outputs_size(),
        (unsigned long long)model_fingerprint_);

    return ESP_OK;
}

bool TflmModelRunner::ready() const
{
    return interpreter_ != nullptr;
}

TfLiteTensor *TflmModelRunner::input(size_t index)
{
    return interpreter_ && index < interpreter_->inputs_size()
        ? interpreter_->input(index)
        : nullptr;
}

TfLiteTensor *TflmModelRunner::output(size_t index)
{
    return interpreter_ && index < interpreter_->outputs_size()
        ? interpreter_->output(index)
        : nullptr;
}

size_t TflmModelRunner::input_count() const
{
    return interpreter_ ? interpreter_->inputs_size() : 0;
}

size_t TflmModelRunner::output_count() const
{
    return interpreter_ ? interpreter_->outputs_size() : 0;
}

esp_err_t TflmModelRunner::invoke(int64_t *duration_us)
{
    if (!interpreter_) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t start_us = esp_timer_get_time();
    const TfLiteStatus status = interpreter_->Invoke();
    const int64_t elapsed = esp_timer_get_time() - start_us;

    if (duration_us) {
        *duration_us = elapsed;
    }

    return status == kTfLiteOk ? ESP_OK : ESP_FAIL;
}

size_t TflmModelRunner::model_size() const
{
    return model_size_;
}

uint64_t TflmModelRunner::model_fingerprint() const
{
    return model_fingerprint_;
}

size_t TflmModelRunner::arena_used_bytes() const
{
    return interpreter_ ? interpreter_->arena_used_bytes() : 0;
}

size_t tflm_tensor_element_count(const TfLiteTensor *tensor)
{
    if (!tensor || !tensor->dims) {
        return 0;
    }

    size_t count = 1;
    for (int i = 0; i < tensor->dims->size; ++i) {
        if (tensor->dims->data[i] <= 0) {
            return 0;
        }
        count *= static_cast<size_t>(tensor->dims->data[i]);
    }
    return count;
}

float tflm_tensor_value_as_float(const TfLiteTensor *tensor, size_t index)
{
    if (!tensor || index >= tflm_tensor_element_count(tensor)) {
        return 0.0f;
    }

    switch (tensor->type) {
    case kTfLiteFloat32:
        return tensor->data.f[index];
    case kTfLiteInt8:
        return (static_cast<int>(tensor->data.int8[index]) -
                tensor->params.zero_point) * tensor->params.scale;
    case kTfLiteUInt8:
        return (static_cast<int>(tensor->data.uint8[index]) -
                tensor->params.zero_point) * tensor->params.scale;
    case kTfLiteInt16:
        return (static_cast<int>(tensor->data.i16[index]) -
                tensor->params.zero_point) * tensor->params.scale;
    default:
        return 0.0f;
    }
}

esp_err_t tflm_tensor_write_real(TfLiteTensor *tensor, size_t index, float value)
{
    if (!tensor || index >= tflm_tensor_element_count(tensor)) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (tensor->type) {
    case kTfLiteFloat32:
        tensor->data.f[index] = value;
        return ESP_OK;

    case kTfLiteInt8: {
        if (tensor->params.scale <= 0.0f) {
            return ESP_ERR_INVALID_STATE;
        }
        int q = static_cast<int>(lrintf(value / tensor->params.scale)) +
                tensor->params.zero_point;
        if (q < -128) q = -128;
        if (q > 127) q = 127;
        tensor->data.int8[index] = static_cast<int8_t>(q);
        return ESP_OK;
    }

    case kTfLiteUInt8: {
        if (tensor->params.scale <= 0.0f) {
            return ESP_ERR_INVALID_STATE;
        }
        int q = static_cast<int>(lrintf(value / tensor->params.scale)) +
                tensor->params.zero_point;
        if (q < 0) q = 0;
        if (q > 255) q = 255;
        tensor->data.uint8[index] = static_cast<uint8_t>(q);
        return ESP_OK;
    }

    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

#endif
