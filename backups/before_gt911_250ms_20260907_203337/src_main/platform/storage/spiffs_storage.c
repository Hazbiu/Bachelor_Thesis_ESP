#include "platform/storage/spiffs_storage.h"

#include <stddef.h>

#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "app_main";


esp_err_t spiffs_storage_mount(void)
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
