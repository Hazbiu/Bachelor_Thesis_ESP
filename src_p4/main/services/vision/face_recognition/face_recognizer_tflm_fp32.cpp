#include "config/app_features.h"

#if APP_FACE_RECOG_BACKEND == APP_AI_BACKEND_TFLM_FP32

#include "services/vision/backends/face_recognizer_backend.h"

#include <algorithm>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <string>
#include <utility>
#include <vector>

#include "config/app_config.h"
#include "diagnostics/ai_pipeline_status.h"
#include "diagnostics/core_trace.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "services/vision/backends/tflm_model_runner.hpp"
#include "services/vision/face_detector.h"

static const char *TAG = "face_recog_tflm_fp32";
static TflmModelRunner s_runner;

struct embedding_entry_t {
    std::string name;
    std::vector<float> embedding;
};

struct enrollment_image_t {
    std::string person_name;
    std::string file_name;
    std::string full_path;
};

struct embedding_timing_t {
    int64_t preprocess_us;
    int64_t invoke_us;
    int64_t postprocess_us;
    int64_t total_us;
};

struct aligned_face_geometry_t {
    float center_x;
    float center_y;
    float size;
    float angle;
    bool eye_aligned;
};

static std::vector<embedding_entry_t> s_entries;
static size_t s_embedding_dim = 0;

static constexpr char DB_MAGIC[8] = {
    'T', 'F', '3', '2', 'D', 'B', '2', '\0'
};
static constexpr uint32_t DB_VERSION = 2U;

struct __attribute__((packed)) tflm_fp32_db_header_t {
    char magic[8];
    uint32_t version;
    uint32_t embedding_dim;
    uint32_t entry_count;
    uint64_t fingerprint;
};

struct __attribute__((packed)) tflm_fp32_db_entry_prefix_t {
    char name[FACE_RECOG_MAX_NAME_LEN];
};

static bool exact_recognition_input(const TfLiteTensor *tensor)
{
    return tensor &&
           tensor->type == kTfLiteFloat32 &&
           tensor->dims &&
           tensor->dims->size == 4 &&
           tensor->dims->data[0] == 1 &&
           tensor->dims->data[1] == APP_TFLM_MOBILEFACENET_INPUT_SIZE &&
           tensor->dims->data[2] == APP_TFLM_MOBILEFACENET_INPUT_SIZE &&
           tensor->dims->data[3] == 3;
}

static bool exact_embedding_output(const TfLiteTensor *tensor)
{
    return tensor &&
           tensor->type == kTfLiteFloat32 &&
           tflm_tensor_element_count(tensor) ==
               APP_TFLM_MOBILEFACENET_EMBEDDING_DIM;
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

static bool sample_bilinear(
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
    if (x < 0.0f || y < 0.0f ||
        x > static_cast<float>(width - 1U) ||
        y > static_cast<float>(height - 1U)) {
        *r = 0.0f;
        *g = 0.0f;
        *b = 0.0f;
        return false;
    }

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
    return true;
}

static inline float normalize_mobilefacenet_pixel(float value)
{
    return value / 127.5f - 1.0f;
}

static aligned_face_geometry_t face_alignment_geometry(
    const face_box_t *box,
    uint32_t width,
    uint32_t height)
{
    aligned_face_geometry_t geometry = {};

    /*
     * BlazeFace keypoints 0 and 1 are the two eyes. ESP-DL also supplies the
     * eye landmarks at the start of its 5-point set. Sort by image x position
     * so the alignment direction is stable regardless of anatomical naming.
     */
    if (box && box->keypoint_count >= 4) {
        float x0 = static_cast<float>(box->keypoints[0]);
        float y0 = static_cast<float>(box->keypoints[1]);
        float x1 = static_cast<float>(box->keypoints[2]);
        float y1 = static_cast<float>(box->keypoints[3]);

        if (x0 > x1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }

        const float dx = x1 - x0;
        const float dy = y1 - y0;
        const float eye_distance = sqrtf(dx * dx + dy * dy);

        if (eye_distance >= 4.0f) {
            const float angle = atan2f(dy, dx);
            const float size =
                eye_distance * APP_TFLM_RECOG_EYE_DISTANCE_SCALE;
            const float eye_center_x = (x0 + x1) * 0.5f;
            const float eye_center_y = (y0 + y1) * 0.5f;
            const float down =
                size * APP_TFLM_RECOG_EYE_CENTER_DOWN_SHIFT;

            geometry.center_x =
                eye_center_x - down * sinf(angle);
            geometry.center_y =
                eye_center_y + down * cosf(angle);
            geometry.size = size;
            geometry.angle = angle;
            geometry.eye_aligned = true;
            return geometry;
        }
    }

    /*
     * Fallback for a detector without usable eye landmarks. This path should
     * not normally be used with either configured backend.
     */
    const int x1 = box ? std::max(0, box->x1) : 0;
    const int y1 = box ? std::max(0, box->y1) : 0;
    const int x2 = box
        ? std::min(static_cast<int>(width) - 1, box->x2)
        : static_cast<int>(width) - 1;
    const int y2 = box
        ? std::min(static_cast<int>(height) - 1, box->y2)
        : static_cast<int>(height) - 1;

    const float bw = static_cast<float>(std::max(1, x2 - x1 + 1));
    const float bh = static_cast<float>(std::max(1, y2 - y1 + 1));

    geometry.center_x = (static_cast<float>(x1 + x2)) * 0.5f;
    geometry.center_y = (static_cast<float>(y1 + y2)) * 0.5f;
    geometry.size = std::max(bw, bh) * 1.35f;
    geometry.angle = 0.0f;
    geometry.eye_aligned = false;
    return geometry;
}

static esp_err_t fill_mobilefacenet_input(
    const uint8_t *buf,
    uint32_t width,
    uint32_t height,
    const face_box_t *box,
    bool rgb565,
    bool *used_eye_alignment)
{
    TfLiteTensor *input = s_runner.input(0);
    if (!buf || !box || width == 0 || height == 0 ||
        !exact_recognition_input(input)) {
        return ESP_ERR_INVALID_ARG;
    }

    const aligned_face_geometry_t geometry =
        face_alignment_geometry(box, width, height);

    if (geometry.size <= 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    if (used_eye_alignment) {
        *used_eye_alignment = geometry.eye_aligned;
    }

    const float c = cosf(geometry.angle);
    const float s = sinf(geometry.angle);
    const float output_size =
        static_cast<float>(APP_TFLM_MOBILEFACENET_INPUT_SIZE);

    float *out = input->data.f;
    size_t index = 0;

    for (int dy = 0; dy < APP_TFLM_MOBILEFACENET_INPUT_SIZE; ++dy) {
        const float local_y =
            ((static_cast<float>(dy) + 0.5f) / output_size - 0.5f) *
            geometry.size;

        for (int dx = 0; dx < APP_TFLM_MOBILEFACENET_INPUT_SIZE; ++dx) {
            const float local_x =
                ((static_cast<float>(dx) + 0.5f) / output_size - 0.5f) *
                geometry.size;

            /*
             * Inverse affine sampling: rotate the canonical output square by
             * the detected eye angle into the source image.
             */
            const float source_x =
                geometry.center_x + c * local_x - s * local_y;
            const float source_y =
                geometry.center_y + s * local_x + c * local_y;

            float r, g, b;
            sample_bilinear(
                buf, width, height,
                source_x, source_y,
                rgb565,
                &r, &g, &b);

            out[index++] = normalize_mobilefacenet_pixel(r);
            out[index++] = normalize_mobilefacenet_pixel(g);
            out[index++] = normalize_mobilefacenet_pixel(b);
        }
    }

    return ESP_OK;
}

static esp_err_t compute_embedding(
    const uint8_t *buf,
    uint32_t width,
    uint32_t height,
    const face_box_t *box,
    bool rgb565,
    std::vector<float> *embedding,
    embedding_timing_t *timing)
{
    if (!embedding || !timing) {
        return ESP_ERR_INVALID_ARG;
    }

    *timing = {};
    const int64_t start_us = esp_timer_get_time();

    bool eye_aligned = false;
    const esp_err_t preprocess_ret = fill_mobilefacenet_input(
        buf, width, height, box, rgb565, &eye_aligned);
    if (preprocess_ret != ESP_OK) {
        return preprocess_ret;
    }

    const int64_t after_preprocess_us = esp_timer_get_time();

    int64_t invoke_us = 0;
    const esp_err_t invoke_ret = s_runner.invoke(&invoke_us);
    if (invoke_ret != ESP_OK) {
        return invoke_ret;
    }

    TfLiteTensor *output = s_runner.output(0);
    if (!exact_embedding_output(output)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    embedding->resize(APP_TFLM_MOBILEFACENET_EMBEDDING_DIM);

    float norm_sq = 0.0f;
    for (size_t i = 0;
         i < APP_TFLM_MOBILEFACENET_EMBEDDING_DIM;
         ++i) {
        const float value = output->data.f[i];
        (*embedding)[i] = value;
        norm_sq += value * value;
    }

    const float norm = sqrtf(norm_sq);
    if (norm <= 1e-12f || !isfinite(norm)) {
        return ESP_FAIL;
    }

    const float inv_norm = 1.0f / norm;
    for (float &value : *embedding) {
        value *= inv_norm;
    }

    const int64_t end_us = esp_timer_get_time();

    timing->preprocess_us = after_preprocess_us - start_us;
    timing->invoke_us = invoke_us;
    timing->postprocess_us =
        end_us - after_preprocess_us - invoke_us;
    timing->total_us = end_us - start_us;

    ESP_LOGD(
        TAG,
        "Embedding alignment=%s",
        eye_aligned ? "eyes" : "box-fallback");

    return ESP_OK;
}

static float cosine_similarity(
    const std::vector<float> &a,
    const std::vector<float> &b)
{
    if (a.size() != b.size() || a.empty()) {
        return -1.0f;
    }

    float dot = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
    }

    /* Both vectors are already L2-normalized. */
    return dot;
}

static uint64_t fnv1a_update(
    uint64_t hash,
    const void *data,
    size_t length)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t fnv1a_update_cstring(
    uint64_t hash,
    const char *value)
{
    if (value) {
        hash = fnv1a_update(hash, value, strlen(value));
    }
    const uint8_t separator = 0;
    return fnv1a_update(hash, &separator, 1);
}

static uint64_t fnv1a_update_string(
    uint64_t hash,
    const std::string &value)
{
    return fnv1a_update_cstring(hash, value.c_str());
}

static bool path_is_directory(const std::string &path)
{
    struct stat st = {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool has_rgb_extension(const char *name)
{
    if (!name) {
        return false;
    }

    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".rgb") == 0;
}

static esp_err_t hash_file(
    const std::string &path,
    uint64_t *hash)
{
    if (!hash) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path.c_str(), "rb");
    if (!file) {
        return ESP_FAIL;
    }

    uint8_t buffer[4096];
    size_t n = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        *hash = fnv1a_update(*hash, buffer, n);
    }

    const bool failed = ferror(file) != 0;
    fclose(file);
    return failed ? ESP_FAIL : ESP_OK;
}

static esp_err_t collect_enrollment_images(
    std::vector<enrollment_image_t> *images,
    uint64_t *fingerprint)
{
    if (!images || !fingerprint) {
        return ESP_ERR_INVALID_ARG;
    }

    images->clear();
    *fingerprint = UINT64_C(14695981039346656037);

    const uint64_t recognition_model_fingerprint =
        s_runner.model_fingerprint();
    *fingerprint = fnv1a_update(
        *fingerprint,
        &recognition_model_fingerprint,
        sizeof(recognition_model_fingerprint));

    *fingerprint = fnv1a_update_cstring(
        *fingerprint,
        "TFLM_FP32_MOBILEFACENET_ALIGNMENT_V2");
    *fingerprint = fnv1a_update_cstring(
        *fingerprint,
        face_detect_backend_name());

#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_FP32
    /*
     * Enrollment boxes/eyes come from BlazeFace. Include that model file in the
     * database fingerprint so changing the detector rebuilds the embeddings.
     */
    (void)hash_file(APP_TFLM_FP32_FACE_DETECT_MODEL_PATH, fingerprint);
#endif

    DIR *root = opendir(APP_FACE_ENROLL_ROOT);
    if (!root) {
        ESP_LOGW(
            TAG,
            "Enrollment root unavailable: %s",
            APP_FACE_ENROLL_ROOT);
        return ESP_ERR_NOT_FOUND;
    }

    std::vector<std::string> people;
    struct dirent *entry = nullptr;

    while ((entry = readdir(root)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        std::string person(entry->d_name);
        std::string person_path =
            std::string(APP_FACE_ENROLL_ROOT) + "/" + person;

        if (!path_is_directory(person_path)) {
            continue;
        }

        if (person.size() >= FACE_RECOG_MAX_NAME_LEN) {
            ESP_LOGW(
                TAG,
                "Skipping overlong person folder: %s",
                person.c_str());
            continue;
        }

        people.push_back(person);
    }

    closedir(root);
    std::sort(people.begin(), people.end());

    const size_t expected_size =
        static_cast<size_t>(APP_FACE_ENROLL_WIDTH) *
        APP_FACE_ENROLL_HEIGHT * 3U;

    for (const std::string &person : people) {
        const std::string person_path =
            std::string(APP_FACE_ENROLL_ROOT) + "/" + person;

        DIR *dir = opendir(person_path.c_str());
        if (!dir) {
            continue;
        }

        std::vector<std::string> rgb_files;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.' ||
                !has_rgb_extension(entry->d_name)) {
                continue;
            }
            rgb_files.emplace_back(entry->d_name);
        }

        closedir(dir);
        std::sort(rgb_files.begin(), rgb_files.end());

        for (const std::string &file_name : rgb_files) {
            const std::string full_path =
                person_path + "/" + file_name;

            struct stat st = {};
            if (stat(full_path.c_str(), &st) != 0 ||
                !S_ISREG(st.st_mode) ||
                static_cast<size_t>(st.st_size) != expected_size) {
                ESP_LOGW(
                    TAG,
                    "Skipping invalid enrollment RGB: %s",
                    full_path.c_str());
                continue;
            }

            *fingerprint =
                fnv1a_update_string(*fingerprint, person);
            *fingerprint =
                fnv1a_update_string(*fingerprint, file_name);

            if (hash_file(full_path, fingerprint) != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "Could not fingerprint enrollment file: %s",
                    full_path.c_str());
                continue;
            }

            images->push_back(
                {person, file_name, full_path});
        }
    }

    ESP_LOGI(
        TAG,
        "TFLM-FP32 enrollment scan: people=%u images=%u fingerprint=%016llx",
        (unsigned)people.size(),
        (unsigned)images->size(),
        (unsigned long long)*fingerprint);

    return ESP_OK;
}

static esp_err_t read_rgb888_file(
    const char *path,
    uint8_t **out_data)
{
    if (!path || !out_data) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_data = nullptr;

    const size_t bytes =
        static_cast<size_t>(APP_FACE_ENROLL_WIDTH) *
        APP_FACE_ENROLL_HEIGHT * 3U;

    uint8_t *data = static_cast<uint8_t *>(
        heap_caps_malloc(
            bytes,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data) {
        return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        heap_caps_free(data);
        return ESP_FAIL;
    }

    const size_t got = fread(data, 1, bytes, file);
    fclose(file);

    if (got != bytes) {
        heap_caps_free(data);
        return ESP_FAIL;
    }

    *out_data = data;
    return ESP_OK;
}

static int largest_face(
    const face_box_t *boxes,
    int count)
{
    int best = -1;
    int best_area = -1;

    for (int i = 0; i < count; ++i) {
        if (boxes[i].keypoint_count < 4) {
            continue;
        }

        const int w = std::max(0, boxes[i].x2 - boxes[i].x1);
        const int h = std::max(0, boxes[i].y2 - boxes[i].y1);
        const int area = w * h;

        if (area > best_area) {
            best_area = area;
            best = i;
        }
    }

    return best;
}

static esp_err_t save_database(uint64_t fingerprint)
{
    FILE *file = fopen(APP_TFLM_FP32_RECOG_DATABASE_PATH, "wb");
    if (!file) {
        return ESP_FAIL;
    }

    tflm_fp32_db_header_t header = {};
    memcpy(header.magic, DB_MAGIC, sizeof(DB_MAGIC));
    header.version = DB_VERSION;
    header.embedding_dim =
        static_cast<uint32_t>(s_embedding_dim);
    header.entry_count =
        static_cast<uint32_t>(s_entries.size());
    header.fingerprint = fingerprint;

    if (fwrite(&header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return ESP_FAIL;
    }

    for (const embedding_entry_t &entry : s_entries) {
        tflm_fp32_db_entry_prefix_t prefix = {};
        snprintf(
            prefix.name,
            sizeof(prefix.name),
            "%s",
            entry.name.c_str());

        if (fwrite(&prefix, 1, sizeof(prefix), file) != sizeof(prefix) ||
            fwrite(
                entry.embedding.data(),
                sizeof(float),
                s_embedding_dim,
                file) != s_embedding_dim) {
            fclose(file);
            return ESP_FAIL;
        }
    }

    fclose(file);
    return ESP_OK;
}

static bool load_database(
    uint64_t fingerprint,
    size_t expected_dim)
{
    FILE *file = fopen(APP_TFLM_FP32_RECOG_DATABASE_PATH, "rb");
    if (!file) {
        return false;
    }

    tflm_fp32_db_header_t header = {};
    if (fread(&header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header.magic, DB_MAGIC, sizeof(DB_MAGIC)) != 0 ||
        header.version != DB_VERSION ||
        header.embedding_dim != expected_dim ||
        header.fingerprint != fingerprint ||
        header.entry_count > 10000U) {
        fclose(file);
        return false;
    }

    std::vector<embedding_entry_t> loaded;
    loaded.reserve(header.entry_count);

    for (uint32_t i = 0; i < header.entry_count; ++i) {
        tflm_fp32_db_entry_prefix_t prefix = {};
        if (fread(&prefix, 1, sizeof(prefix), file) != sizeof(prefix)) {
            fclose(file);
            return false;
        }

        prefix.name[FACE_RECOG_MAX_NAME_LEN - 1] = '\0';

        embedding_entry_t entry;
        entry.name = prefix.name;
        entry.embedding.resize(expected_dim);

        if (fread(
                entry.embedding.data(),
                sizeof(float),
                expected_dim,
                file) != expected_dim) {
            fclose(file);
            return false;
        }

        loaded.push_back(std::move(entry));
    }

    fclose(file);
    s_entries.swap(loaded);
    s_embedding_dim = expected_dim;
    return true;
}

static esp_err_t rebuild_database(
    const std::vector<enrollment_image_t> &images,
    uint64_t fingerprint)
{
    s_entries.clear();
    s_embedding_dim =
        APP_TFLM_MOBILEFACENET_EMBEDDING_DIM;

    unsigned rejected = 0;

    for (const enrollment_image_t &source : images) {
        uint8_t *rgb = nullptr;
        if (read_rgb888_file(source.full_path.c_str(), &rgb) != ESP_OK) {
            rejected++;
            continue;
        }

        face_box_t boxes[5] = {};
        const int face_count = face_detect_run_rgb888(
            rgb,
            APP_FACE_ENROLL_WIDTH,
            APP_FACE_ENROLL_HEIGHT,
            boxes,
            5);

        const int best = largest_face(boxes, face_count);
        if (best < 0) {
            ESP_LOGW(
                TAG,
                "Enrollment rejected: person=%s file=%s no face/eyes",
                source.person_name.c_str(),
                source.file_name.c_str());
            heap_caps_free(rgb);
            rejected++;
            continue;
        }

        std::vector<float> embedding;
        embedding_timing_t timing = {};

        const esp_err_t embedding_ret = compute_embedding(
            rgb,
            APP_FACE_ENROLL_WIDTH,
            APP_FACE_ENROLL_HEIGHT,
            &boxes[best],
            false,
            &embedding,
            &timing);

        heap_caps_free(rgb);

        /*
         * TFLM-FP32 MobileFaceNet can monopolize CPU1 for several seconds.
         * Yield between enrollment samples so the CPU1 idle task can run
         * and service the Task Watchdog.
         */
        vTaskDelay(pdMS_TO_TICKS(10));

        if (embedding_ret != ESP_OK ||
            embedding.size() != s_embedding_dim) {
            ESP_LOGW(
                TAG,
                "Enrollment embedding failed: person=%s file=%s error=%s",
                source.person_name.c_str(),
                source.file_name.c_str(),
                esp_err_to_name(embedding_ret));
            rejected++;
            continue;
        }

        s_entries.push_back(
            {source.person_name, std::move(embedding)});

        ESP_LOGI(
            TAG,
            "[AI-BENCH] backend=TFLM-FP32 model=MobileFaceNet "
            "stage=ENROLL_EMBED total_us=%lld invoke_us=%lld person=%s",
            (long long)timing.total_us,
            (long long)timing.invoke_us,
            source.person_name.c_str());
    }

    if (s_entries.empty()) {
        ESP_LOGE(
            TAG,
            "TFLM-FP32 database rebuild produced zero embeddings");
        return ESP_FAIL;
    }

    const esp_err_t save_ret = save_database(fingerprint);
    if (save_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not save %s: %s",
            APP_TFLM_FP32_RECOG_DATABASE_PATH,
            esp_err_to_name(save_ret));
        return save_ret;
    }

    ESP_LOGI(
        TAG,
        "TFLM-FP32 database rebuilt: entries=%u rejected=%u dim=%u path=%s",
        (unsigned)s_entries.size(),
        rejected,
        (unsigned)s_embedding_dim,
        APP_TFLM_FP32_RECOG_DATABASE_PATH);

    return ESP_OK;
}

extern "C" esp_err_t tflm_fp32_face_recognition_init(void)
{
    const esp_err_t init_ret = s_runner.init(
        APP_TFLM_FP32_FACE_RECOG_MODEL_PATH,
        APP_TFLM_RECOG_TENSOR_ARENA_BYTES);

    if (init_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "MobileFaceNet model initialization failed: path=%s error=%s",
            APP_TFLM_FP32_FACE_RECOG_MODEL_PATH,
            esp_err_to_name(init_ret));
        return init_ret;
    }

    TfLiteTensor *input = s_runner.input(0);
    TfLiteTensor *output = s_runner.output(0);

    if (!exact_recognition_input(input) ||
        !exact_embedding_output(output)) {
        ESP_LOGE(
            TAG,
            "Wrong MobileFaceNet tensor contract. Required: "
            "input=[1,112,112,3] F32, output0=[1,192] F32");
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_embedding_dim =
        APP_TFLM_MOBILEFACENET_EMBEDDING_DIM;

    ESP_LOGI(
        TAG,
        "TFLM-FP32 MobileFaceNet ready: model=%s dim=%u arena_used=%u",
        APP_TFLM_FP32_FACE_RECOG_MODEL_PATH,
        (unsigned)s_embedding_dim,
        (unsigned)s_runner.arena_used_bytes());

    std::vector<enrollment_image_t> images;
    uint64_t fingerprint = 0;

    const esp_err_t scan_ret =
        collect_enrollment_images(&images, &fingerprint);

    if (scan_ret != ESP_OK || images.empty()) {
        ESP_LOGW(
            TAG,
            "No enrollment images available; recognition database is empty");
        s_entries.clear();
        return ESP_OK;
    }

    if (load_database(fingerprint, s_embedding_dim)) {
        ESP_LOGI(
            TAG,
            "TFLM-FP32 database loaded: entries=%u dim=%u path=%s",
            (unsigned)s_entries.size(),
            (unsigned)s_embedding_dim,
            APP_TFLM_FP32_RECOG_DATABASE_PATH);
        return ESP_OK;
    }

    return rebuild_database(images, fingerprint);
}

extern "C" int tflm_fp32_face_recognition_get_count(void)
{
    return static_cast<int>(s_entries.size());
}

extern "C" esp_err_t tflm_fp32_face_recognition_recognize(
    uint8_t *camera_buf,
    uint32_t width,
    uint32_t height,
    const face_box_t *box,
    char *out_name,
    int out_name_len,
    float *out_score)
{
    if (!s_runner.ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!camera_buf || !box || !out_name ||
        out_name_len <= 0 || !out_score) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(out_name, out_name_len, "%s", "unknown");
    *out_score = 0.0f;

    if (s_entries.empty()) {
        return ESP_OK;
    }

#if APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565
    const bool rgb565 = true;
#else
    const bool rgb565 = false;
#endif

    std::vector<float> query;
    embedding_timing_t timing = {};

    core_trace(TAG, "RECOGNITION_BEGIN");

    const esp_err_t embedding_ret = compute_embedding(
        camera_buf,
        width,
        height,
        box,
        rgb565,
        &query,
        &timing);

    if (embedding_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Live MobileFaceNet embedding failed: %s",
            esp_err_to_name(embedding_ret));
        return embedding_ret;
    }

    diagnostics_ai_live_metrics_record_recognition(
        timing.invoke_us > 0 ? (uint64_t)timing.invoke_us : 0U);

    ESP_LOGI(
        TAG,
        "[AI-BENCH] backend=TFLM-FP32 model=MobileFaceNet "
        "stage=RECOGNITION cpu=%d preprocess_us=%lld invoke_us=%lld "
        "postprocess_us=%lld total_us=%lld",
        xPortGetCoreID(),
        (long long)timing.preprocess_us,
        (long long)timing.invoke_us,
        (long long)timing.postprocess_us,
        (long long)timing.total_us);

    float best_score = -1.0f;
    const char *best_name = nullptr;

    for (const embedding_entry_t &entry : s_entries) {
        const float similarity =
            cosine_similarity(query, entry.embedding);

        if (similarity > best_score) {
            best_score = similarity;
            best_name = entry.name.c_str();
        }
    }

    *out_score = best_score;

    if (best_name &&
        best_score >= APP_TFLM_RECOG_SIMILARITY_THRESHOLD) {
        snprintf(out_name, out_name_len, "%s", best_name);
    }

    ESP_LOGI(
        TAG,
        "TFLM-FP32 recognition: person=%s similarity=%.3f threshold=%.3f",
        out_name,
        (double)best_score,
        (double)APP_TFLM_RECOG_SIMILARITY_THRESHOLD);

    return ESP_OK;
}

#else

#include "services/vision/backends/face_recognizer_backend.h"

extern "C" esp_err_t tflm_fp32_face_recognition_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

extern "C" int tflm_fp32_face_recognition_get_count(void)
{
    return 0;
}

extern "C" esp_err_t tflm_fp32_face_recognition_recognize(
    uint8_t *,
    uint32_t,
    uint32_t,
    const face_box_t *,
    char *,
    int,
    float *)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
