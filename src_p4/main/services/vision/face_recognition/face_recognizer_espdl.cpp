#include "services/vision/face_recognizer.h"
#include "services/vision/backends/face_recognizer_backend.h"

#include <algorithm>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <list>
#include <string>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dl_detect_define.hpp"
#include "dl_image_define.hpp"
#include "human_face_recognition.hpp"

#include "diagnostics/ai_pipeline_status.h"
#include "diagnostics/core_trace.h"
#include "platform/camera/video_capture.h"
#include "services/vision/face_detector.h"

static const char *TAG_RECOG = "face_recognition_wrapper";

static HumanFaceRecognizer *s_recognizer = nullptr;

static char s_database_path[] = "/sdcard/FACE.DB";

#define FACE_ENROLL_ROOT            "/sdcard/enroll"
#define FACE_ENROLL_WIDTH           320
#define FACE_ENROLL_HEIGHT          240
#define FACE_ENROLL_CHANNELS        3
#define FACE_ENROLL_FILE_SIZE       ((size_t)FACE_ENROLL_WIDTH * FACE_ENROLL_HEIGHT * FACE_ENROLL_CHANNELS)
#define FACE_SIMILARITY_THRESHOLD   0.55f
#define FACE_DB_NAMES_VERSION       1U

/*
 * Espressif's FACE.DB stores feature IDs and feature vectors, but not names.
 * This wrapper appends a small private footer after the Espressif records.
 * Espressif's loader reads exactly num_feats_total records and ignores trailing
 * bytes, so recognition remains compatible with HumanFaceRecognizer.
 *
 * No enrollment call is made after the footer is appended. When the enrollment
 * dataset changes, the complete database is deleted and rebuilt before a new
 * footer is written.
 */
static const char FACE_DB_NAMES_MAGIC[8] = {
    'F', 'A', 'C', 'E', 'N', 'A', 'M', '1'
};

typedef struct __attribute__((packed)) {
    uint16_t id;
    char name[FACE_RECOG_MAX_NAME_LEN];
} face_db_name_record_t;

typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t version;
    uint32_t entry_count;
    uint32_t record_size;
    uint64_t dataset_fingerprint;
} face_db_names_trailer_t;

struct enrollment_image_t {
    std::string person_name;
    std::string file_name;
    std::string full_path;
};

struct face_name_entry_t {
    uint16_t id;
    std::string name;
};

static std::vector<face_name_entry_t> s_name_entries;

static uint64_t fnv1a_update(uint64_t hash, const void *data, size_t length)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);

    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }

    return hash;
}

static uint64_t fnv1a_update_string(uint64_t hash, const std::string &value)
{
    hash = fnv1a_update(hash, value.data(), value.size());
    const uint8_t separator = 0;
    return fnv1a_update(hash, &separator, sizeof(separator));
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

static esp_err_t hash_file_contents(const std::string &path, uint64_t *hash)
{
    if (!hash) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path.c_str(), "rb");
    if (!file) {
        ESP_LOGE(TAG_RECOG, "Cannot read enrollment image: %s", path.c_str());
        return ESP_FAIL;
    }

    uint8_t chunk[4096];
    size_t bytes_read = 0;

    while ((bytes_read = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        *hash = fnv1a_update(*hash, chunk, bytes_read);
    }

    const bool read_failed = ferror(file) != 0;
    fclose(file);

    return read_failed ? ESP_FAIL : ESP_OK;
}

static esp_err_t collect_enrollment_images(
    std::vector<enrollment_image_t> *images,
    uint64_t *dataset_fingerprint)
{
    if (!images || !dataset_fingerprint) {
        return ESP_ERR_INVALID_ARG;
    }

    images->clear();
    *dataset_fingerprint = UINT64_C(14695981039346656037);
    *dataset_fingerprint = fnv1a_update_string(
        *dataset_fingerprint,
        "RGB888:320x240:FACE_DB_V1"
    );

    DIR *root = opendir(FACE_ENROLL_ROOT);
    if (!root) {
        ESP_LOGW(TAG_RECOG, "Enrollment root is unavailable: %s", FACE_ENROLL_ROOT);
        return ESP_ERR_NOT_FOUND;
    }

    std::vector<std::string> person_names;
    struct dirent *person_entry = nullptr;

    while ((person_entry = readdir(root)) != nullptr) {
        if (person_entry->d_name[0] == '.') {
            continue;
        }

        std::string person_name(person_entry->d_name);
        std::string person_path = std::string(FACE_ENROLL_ROOT) + "/" + person_name;

        if (!path_is_directory(person_path)) {
            continue;
        }

        if (person_name.size() >= FACE_RECOG_MAX_NAME_LEN) {
            ESP_LOGW(
                TAG_RECOG,
                "Skipping person folder with a name longer than %d bytes: %s",
                FACE_RECOG_MAX_NAME_LEN - 1,
                person_name.c_str()
            );
            continue;
        }

        person_names.push_back(person_name);
    }

    closedir(root);
    std::sort(person_names.begin(), person_names.end());

    for (const std::string &person_name : person_names) {
        const std::string person_path = std::string(FACE_ENROLL_ROOT) + "/" + person_name;
        DIR *person_dir = opendir(person_path.c_str());

        if (!person_dir) {
            ESP_LOGW(TAG_RECOG, "Cannot open person folder: %s", person_path.c_str());
            continue;
        }

        std::vector<std::string> rgb_file_names;
        struct dirent *image_entry = nullptr;

        while ((image_entry = readdir(person_dir)) != nullptr) {
            if (image_entry->d_name[0] == '.' || !has_rgb_extension(image_entry->d_name)) {
                continue;
            }

            rgb_file_names.emplace_back(image_entry->d_name);
        }

        closedir(person_dir);
        std::sort(rgb_file_names.begin(), rgb_file_names.end());

        for (const std::string &file_name : rgb_file_names) {
            const std::string full_path = person_path + "/" + file_name;
            struct stat st = {};

            if (stat(full_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                ESP_LOGW(TAG_RECOG, "Skipping unreadable enrollment file: %s", full_path.c_str());
                continue;
            }

            if ((size_t)st.st_size != FACE_ENROLL_FILE_SIZE) {
                ESP_LOGW(
                    TAG_RECOG,
                    "Skipping %s: size=%ld, expected=%u bytes",
                    full_path.c_str(),
                    (long)st.st_size,
                    (unsigned)FACE_ENROLL_FILE_SIZE
                );
                continue;
            }

            *dataset_fingerprint = fnv1a_update_string(*dataset_fingerprint, person_name);
            *dataset_fingerprint = fnv1a_update_string(*dataset_fingerprint, file_name);

            esp_err_t hash_ret = hash_file_contents(full_path, dataset_fingerprint);
            if (hash_ret != ESP_OK) {
                ESP_LOGW(TAG_RECOG, "Skipping file that could not be hashed: %s", full_path.c_str());
                continue;
            }

            images->push_back({person_name, file_name, full_path});
        }
    }

    ESP_LOGI(
        TAG_RECOG,
        "Enrollment scan: people=%u valid_rgb_files=%u fingerprint=%016llx",
        (unsigned)person_names.size(),
        (unsigned)images->size(),
        (unsigned long long)*dataset_fingerprint
    );

    return ESP_OK;
}

static bool load_name_footer(
    std::vector<face_name_entry_t> *entries,
    uint64_t *dataset_fingerprint)
{
    if (!entries || !dataset_fingerprint) {
        return false;
    }

    entries->clear();
    *dataset_fingerprint = 0;

    FILE *file = fopen(s_database_path, "rb");
    if (!file) {
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }

    const long file_size = ftell(file);
    if (file_size < (long)sizeof(face_db_names_trailer_t)) {
        fclose(file);
        return false;
    }

    if (fseek(file, file_size - (long)sizeof(face_db_names_trailer_t), SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    face_db_names_trailer_t trailer = {};
    if (fread(&trailer, sizeof(trailer), 1, file) != 1) {
        fclose(file);
        return false;
    }

    if (memcmp(trailer.magic, FACE_DB_NAMES_MAGIC, sizeof(trailer.magic)) != 0 ||
        trailer.version != FACE_DB_NAMES_VERSION ||
        trailer.record_size != sizeof(face_db_name_record_t)) {
        fclose(file);
        return false;
    }

    const uint64_t records_size =
        (uint64_t)trailer.entry_count * sizeof(face_db_name_record_t);

    if (records_size > (uint64_t)file_size - sizeof(face_db_names_trailer_t)) {
        fclose(file);
        return false;
    }

    const long records_offset =
        file_size - (long)sizeof(face_db_names_trailer_t) - (long)records_size;

    if (fseek(file, records_offset, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    entries->reserve(trailer.entry_count);

    for (uint32_t i = 0; i < trailer.entry_count; i++) {
        face_db_name_record_t record = {};

        if (fread(&record, sizeof(record), 1, file) != 1 || record.id == 0) {
            entries->clear();
            fclose(file);
            return false;
        }

        record.name[FACE_RECOG_MAX_NAME_LEN - 1] = '\0';
        if (record.name[0] == '\0') {
            entries->clear();
            fclose(file);
            return false;
        }

        entries->push_back({record.id, std::string(record.name)});
    }

    fclose(file);
    *dataset_fingerprint = trailer.dataset_fingerprint;
    return true;
}

static esp_err_t append_name_footer(uint64_t dataset_fingerprint)
{
    FILE *file = fopen(s_database_path, "ab");
    if (!file) {
        ESP_LOGE(TAG_RECOG, "Cannot append names to %s", s_database_path);
        return ESP_FAIL;
    }

    for (const face_name_entry_t &entry : s_name_entries) {
        face_db_name_record_t record = {};
        record.id = entry.id;
        snprintf(record.name, sizeof(record.name), "%s", entry.name.c_str());

        if (fwrite(&record, sizeof(record), 1, file) != 1) {
            ESP_LOGE(
                TAG_RECOG,
                "Failed to write name record for id=%u",
                (unsigned)entry.id
            );
            fclose(file);
            return ESP_FAIL;
        }
    }

    face_db_names_trailer_t trailer = {};
    memcpy(trailer.magic, FACE_DB_NAMES_MAGIC, sizeof(trailer.magic));
    trailer.version = FACE_DB_NAMES_VERSION;
    trailer.entry_count = (uint32_t)s_name_entries.size();
    trailer.record_size = (uint32_t)sizeof(face_db_name_record_t);
    trailer.dataset_fingerprint = dataset_fingerprint;

    if (fwrite(&trailer, sizeof(trailer), 1, file) != 1) {
        ESP_LOGE(TAG_RECOG, "Failed to write the FACE.DB name trailer");
        fclose(file);
        return ESP_FAIL;
    }

    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        ESP_LOGE(TAG_RECOG, "Failed to flush the FACE.DB name trailer");
        fclose(file);
        return ESP_FAIL;
    }

    fclose(file);
    return ESP_OK;
}

static std::list<dl::detect::result_t> make_detect_result_from_box(const face_box_t *box)
{
    std::list<dl::detect::result_t> detect_res;

    if (!box || box->keypoint_count < 10) {
        return detect_res;
    }

    dl::detect::result_t result = {};
    result.category = 0;
    result.score = box->score;
    result.box = {box->x1, box->y1, box->x2, box->y2};
    result.keypoint.assign(box->keypoints, box->keypoints + 10);
    detect_res.push_back(result);

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

static esp_err_t read_rgb888_image(const std::string &path, uint8_t **out_data)
{
    if (!out_data) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_data = static_cast<uint8_t *>(heap_caps_malloc(
        FACE_ENROLL_FILE_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    ));

    if (!*out_data) {
        ESP_LOGE(TAG_RECOG, "No PSRAM for enrollment image: %s", path.c_str());
        return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(path.c_str(), "rb");
    if (!file) {
        heap_caps_free(*out_data);
        *out_data = nullptr;
        return ESP_FAIL;
    }

    const size_t bytes_read = fread(*out_data, 1, FACE_ENROLL_FILE_SIZE, file);
    const bool read_failed = ferror(file) != 0;
    fclose(file);

    if (read_failed || bytes_read != FACE_ENROLL_FILE_SIZE) {
        ESP_LOGE(
            TAG_RECOG,
            "Short read for %s: %u/%u bytes",
            path.c_str(),
            (unsigned)bytes_read,
            (unsigned)FACE_ENROLL_FILE_SIZE
        );
        heap_caps_free(*out_data);
        *out_data = nullptr;
        return ESP_FAIL;
    }

    return ESP_OK;
}

static int find_largest_face_with_landmarks(const face_box_t *boxes, int count)
{
    int best_index = -1;
    int best_area = -1;

    for (int i = 0; i < count; i++) {
        if (boxes[i].keypoint_count < 10) {
            continue;
        }

        const int width = std::max(0, boxes[i].x2 - boxes[i].x1);
        const int height = std::max(0, boxes[i].y2 - boxes[i].y1);
        const int area = width * height;

        if (area > best_area) {
            best_area = area;
            best_index = i;
        }
    }

    return best_index;
}

static esp_err_t rebuild_database(
    const std::vector<enrollment_image_t> &images,
    uint64_t dataset_fingerprint)
{
    if (s_recognizer) {
        delete s_recognizer;
        s_recognizer = nullptr;
    }

    if (unlink(s_database_path) != 0) {
        struct stat st = {};
        if (stat(s_database_path, &st) == 0) {
            ESP_LOGE(TAG_RECOG, "Could not remove old database: %s", s_database_path);
            return ESP_FAIL;
        }
    }

    s_name_entries.clear();
    s_recognizer = new HumanFaceRecognizer(s_database_path);

    if (!s_recognizer) {
        ESP_LOGE(TAG_RECOG, "Failed to create an empty HumanFaceRecognizer");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG_RECOG,
        "Rebuilding one shared %s from %u RGB images",
        s_database_path,
        (unsigned)images.size()
    );

    unsigned enrolled_count = 0;
    unsigned rejected_count = 0;

    for (const enrollment_image_t &source : images) {
        uint8_t *rgb_data = nullptr;
        esp_err_t read_ret = read_rgb888_image(source.full_path, &rgb_data);

        if (read_ret != ESP_OK) {
            rejected_count++;
            continue;
        }

        face_box_t boxes[5] = {};
        const int face_count = face_detect_run_rgb888(
            rgb_data,
            FACE_ENROLL_WIDTH,
            FACE_ENROLL_HEIGHT,
            boxes,
            5
        );

        const int best_face = find_largest_face_with_landmarks(boxes, face_count);

        if (best_face < 0) {
            ESP_LOGW(
                TAG_RECOG,
                "Enrollment rejected: person=%s file=%s reason=no face with 5 landmarks",
                source.person_name.c_str(),
                source.file_name.c_str()
            );
            heap_caps_free(rgb_data);
            rejected_count++;
            continue;
        }

        dl::image::img_t image = {
            .data = rgb_data,
            .width = FACE_ENROLL_WIDTH,
            .height = FACE_ENROLL_HEIGHT,
            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
        };

        std::list<dl::detect::result_t> detection =
            make_detect_result_from_box(&boxes[best_face]);

        const int count_before = s_recognizer->get_num_feats();
        const esp_err_t enroll_ret = s_recognizer->enroll(image, detection);
        const int count_after = s_recognizer->get_num_feats();
        heap_caps_free(rgb_data);

        if (enroll_ret != ESP_OK || count_after != count_before + 1) {
            ESP_LOGW(
                TAG_RECOG,
                "Enrollment failed: person=%s file=%s error=%s count=%d->%d",
                source.person_name.c_str(),
                source.file_name.c_str(),
                esp_err_to_name(enroll_ret),
                count_before,
                count_after
            );
            rejected_count++;
            continue;
        }

        const uint16_t feature_id = (uint16_t)count_after;
        s_name_entries.push_back({feature_id, source.person_name});
        enrolled_count++;

        ESP_LOGI(
            TAG_RECOG,
            "Enrolled person=%s file=%s feature_id=%u",
            source.person_name.c_str(),
            source.file_name.c_str(),
            (unsigned)feature_id
        );
    }

    if (s_name_entries.empty()) {
        ESP_LOGE(TAG_RECOG, "FACE.DB rebuild failed: no enrollment image produced a feature");
        return ESP_FAIL;
    }

    esp_err_t footer_ret = append_name_footer(dataset_fingerprint);
    if (footer_ret != ESP_OK) {
        return footer_ret;
    }

    ESP_LOGI(
        TAG_RECOG,
        "FACE.DB rebuild complete: enrolled=%u rejected=%u features=%d",
        enrolled_count,
        rejected_count,
        s_recognizer->get_num_feats()
    );

    return ESP_OK;
}

static const char *lookup_name(uint16_t id)
{
    for (const face_name_entry_t &entry : s_name_entries) {
        if (entry.id == id) {
            return entry.name.c_str();
        }
    }

    return nullptr;
}

extern "C" esp_err_t espdl_face_recognition_init(void)
{
    if (s_recognizer) {
        return ESP_OK;
    }

    std::vector<enrollment_image_t> enrollment_images;
    uint64_t current_fingerprint = 0;
    const esp_err_t scan_ret = collect_enrollment_images(
        &enrollment_images,
        &current_fingerprint
    );

    std::vector<face_name_entry_t> stored_names;
    uint64_t stored_fingerprint = 0;
    const bool footer_valid = load_name_footer(
        &stored_names,
        &stored_fingerprint
    );

    const bool dataset_available = scan_ret == ESP_OK && !enrollment_images.empty();
    bool rebuild_required = dataset_available &&
        (!footer_valid || stored_fingerprint != current_fingerprint);

    if (!rebuild_required) {
        s_recognizer = new HumanFaceRecognizer(s_database_path);

        if (!s_recognizer) {
            ESP_LOGE(TAG_RECOG, "Failed to create HumanFaceRecognizer");
            return ESP_ERR_NO_MEM;
        }

        if (footer_valid &&
            (int)stored_names.size() == s_recognizer->get_num_feats()) {
            s_name_entries = stored_names;
        } else if (dataset_available) {
            ESP_LOGW(
                TAG_RECOG,
                "FACE.DB feature/name count mismatch; rebuilding database"
            );
            rebuild_required = true;
        } else {
            s_name_entries.clear();
            ESP_LOGW(
                TAG_RECOG,
                "FACE.DB has no usable embedded name map and enrollment data is unavailable"
            );
        }
    }

    if (rebuild_required) {
        esp_err_t rebuild_ret = rebuild_database(
            enrollment_images,
            current_fingerprint
        );

        if (rebuild_ret != ESP_OK) {
            return rebuild_ret;
        }
    }

    ESP_LOGI(
        TAG_RECOG,
        "HumanFaceRecognizer ready: database=%s features=%d names=%u",
        s_database_path,
        s_recognizer ? s_recognizer->get_num_feats() : 0,
        (unsigned)s_name_entries.size()
    );

    for (const face_name_entry_t &entry : s_name_entries) {
        ESP_LOGI(
            TAG_RECOG,
            "FACE.DB map: feature_id=%u person=%s",
            (unsigned)entry.id,
            entry.name.c_str()
        );
    }

    return s_recognizer ? ESP_OK : ESP_FAIL;
}

extern "C" int espdl_face_recognition_get_count(void)
{
    if (!s_recognizer) {
        return 0;
    }

    return s_recognizer->get_num_feats();
}

extern "C" esp_err_t espdl_face_recognition_recognize(
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

    snprintf(out_name, out_name_len, "%s", "unknown");
    *out_score = 0.0f;

    if (s_recognizer->get_num_feats() <= 0) {
        ESP_LOGW(TAG_RECOG, "Recognition skipped: no features in %s", s_database_path);
        return ESP_OK;
    }

    std::list<dl::detect::result_t> detect_res = make_detect_result_from_box(box);
    if (detect_res.empty()) {
        ESP_LOGW(TAG_RECOG, "Recognition skipped: detector returned no facial landmarks");
        return ESP_OK;
    }

    dl::image::img_t img = make_camera_image(camera_buf, width, height);

    core_trace(TAG_RECOG, "RECOGNITION_BEGIN");
    const int64_t start_us = esp_timer_get_time();

    std::vector<dl::recognition::result_t> results =
        s_recognizer->recognize(img, detect_res);

    const int64_t inference_us = esp_timer_get_time() - start_us;
    diagnostics_ai_live_metrics_record_recognition(
        inference_us > 0 ? (uint64_t)inference_us : 0U);

    ESP_LOGI(
        TAG_RECOG,
        "[CORE-PROOF] backend=ESP-DL RECOGNITION_END cpu=%d duration_us=%lld",
        xPortGetCoreID(),
        (long long)inference_us
    );

    if (results.empty()) {
        ESP_LOGI(TAG_RECOG, "Recognition result: unknown, no database match");
        return ESP_OK;
    }

    const uint16_t feature_id = results[0].id;
    const float similarity = results[0].similarity;
    *out_score = similarity;

    if (similarity < FACE_SIMILARITY_THRESHOLD) {
        ESP_LOGI(
            TAG_RECOG,
            "Face rejected: id=%u similarity=%.3f threshold=%.2f",
            (unsigned)feature_id,
            similarity,
            FACE_SIMILARITY_THRESHOLD
        );
        return ESP_OK;
    }

    const char *person_name = lookup_name(feature_id);
    if (!person_name) {
        ESP_LOGW(
            TAG_RECOG,
            "Face matched feature_id=%u but that ID has no stored person name",
            (unsigned)feature_id
        );
        return ESP_OK;
    }

    snprintf(out_name, out_name_len, "%s", person_name);

    ESP_LOGI(
        TAG_RECOG,
        "Recognized feature_id=%u person=%s similarity=%.3f threshold=%.2f",
        (unsigned)feature_id,
        person_name,
        similarity,
        FACE_SIMILARITY_THRESHOLD
    );

    return ESP_OK;
}
