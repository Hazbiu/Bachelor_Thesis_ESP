#include "services/vision/face_recognition_result_processor.h"

#include <stdio.h>

#include "services/vision/face_result_store.h"


void vision_face_recognition_result_normalize_and_publish(
    int face_index,
    esp_err_t recognition_result,
    char *name,
    size_t name_capacity,
    float *score)
{
    if (recognition_result != ESP_OK) {
        snprintf(name, name_capacity, "%s", "unknown");
        *score = 0.0f;
    }

    vision_face_result_store_publish_recognition_result(
        face_index,
        name,
        *score);
}
