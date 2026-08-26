#include <stdio.h>

#include "app/app_boot.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "services/vision/face_detector.h"
#include "services/vision/face_recognizer.h"
#include "config/app_features.h"

static const char *TAG = "app_main";

static esp_err_t mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0;
    size_t used = 0;
    ret = esp_spiffs_info("storage", &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: total=%d used=%d", total, used);
    } else {
        ESP_LOGE(TAG, "SPIFFS info failed: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

void app_boot_initialize_services(void)
{
    ESP_ERROR_CHECK(mount_spiffs());

    /* SD CARD TEST */
    /* Settings may already have mounted the card to refresh authorized users. */
    esp_err_t sd_ret = bsp_sdcard != NULL ? ESP_OK : bsp_sdcard_mount();
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

