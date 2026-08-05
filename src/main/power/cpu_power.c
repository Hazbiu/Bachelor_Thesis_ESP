#include "power_save/cpu_power.h"

#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"

static const char *TAG = "cpu_power";

#define CPU_MIN_FREQ_MHZ 40
#define CPU_MAX_FREQ_MHZ 360

static esp_pm_lock_handle_t s_ai_cpu_lock = NULL;
static bool s_initialized = false;

esp_err_t cpu_power_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /*
     * Dynamic frequency scaling is enabled, but automatic Light-sleep
     * remains disabled because the application uses the display,
     * MIPI-CSI camera, touchscreen and LVGL.
     */
    const esp_pm_config_t config = {
        .max_freq_mhz = CPU_MAX_FREQ_MHZ,
        .min_freq_mhz = CPU_MIN_FREQ_MHZ,
        .light_sleep_enable = false,
    };

    esp_err_t ret = esp_pm_configure(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_pm_configure failed: %s",
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * This lock requests 360 MHz while AI inference is executing.
     */
    ret = esp_pm_lock_create(
        ESP_PM_CPU_FREQ_MAX,
        0,
        "ai_inference",
        &s_ai_cpu_lock);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Creating AI CPU lock failed: %s",
            esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;

    ESP_LOGI(
        TAG,
        "Dynamic CPU scaling configured: min=%d MHz, max=%d MHz",
        CPU_MIN_FREQ_MHZ,
        CPU_MAX_FREQ_MHZ);

    return ESP_OK;
}

esp_err_t cpu_power_ai_begin(void)
{
    if (!s_initialized || s_ai_cpu_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_pm_lock_acquire(s_ai_cpu_lock);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Acquiring AI CPU lock failed: %s",
            esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t cpu_power_ai_end(void)
{
    if (!s_initialized || s_ai_cpu_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_pm_lock_release(s_ai_cpu_lock);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Releasing AI CPU lock failed: %s",
            esp_err_to_name(ret));
    }

    return ret;
}
