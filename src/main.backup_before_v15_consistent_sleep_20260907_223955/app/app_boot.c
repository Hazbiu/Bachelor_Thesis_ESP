#include <stdio.h>

#include "app/app_boot.h"
#include "platform/storage/sd_card_storage.h"
#include "platform/storage/spiffs_storage.h"
#include "esp_err.h"
#include "esp_log.h"
#include "services/vision/face_detector.h"
#include "services/vision/face_recognizer.h"
#include "config/app_features.h"

static const char *TAG = "app_main";

void app_boot_initialize_services(void)
{
    ESP_ERROR_CHECK(spiffs_storage_mount());

    /* SD CARD TEST */
    /* Settings may already have mounted the card to refresh authorized users. */
    esp_err_t sd_ret = sd_card_storage_ensure_mounted();
    if (sd_ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted at /sdcard");

        FILE *f = fopen("/sdcard/test.txt", "w");
        if (f) {
            fprintf(f, "ESP32-P4 SD card test OK\n");
            fclose(f);
            ESP_LOGI(TAG, "Wrote /sdcard/test.txt");
        } else {
            ESP_LOGE(TAG, "Failed to open /sdcard/test.txt");
        }
    } else {
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(sd_ret));
    }
    /* END SD CARD TEST */

    /*
     * Always print one backend marker even when normal application/AI logs are
     * disabled. This lets a Joulescope trace be associated with the flashed
     * firmware without enabling noisy per-frame logging.
     */
    printf(
        "\n[AI-BENCH-CONFIG] detector=%s recognizer=%s\n",
        face_detect_backend_name(),
        face_recognition_backend_name());

    ESP_LOGI(
        TAG,
        "AI backend selection: detector=%s recognizer=%s",
        face_detect_backend_name(),
        face_recognition_backend_name());

    ESP_ERROR_CHECK(face_detect_init());
    ESP_ERROR_CHECK(face_recognition_init());

    ESP_LOGI(
        TAG,
        "AI services ready: detector=%s recognizer=%s features=%d",
        face_detect_backend_name(),
        face_recognition_backend_name(),
        face_recognition_get_count());
}
