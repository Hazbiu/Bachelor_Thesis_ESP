#include "power_save/cpu_power.h"

#include <stdbool.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"

static const char *TAG = "cpu_power";

/*
 * Dynamic-frequency-scaling limits while the application is awake.
 *
 * The CPU is clock-gated during actual Light-sleep, so these values affect
 * only active execution before sleep and after wake-up. AI inference uses the
 * maximum-frequency lock below and therefore runs at the CPU frequency selected
 * by CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ. When no frequency lock is held, DFS may
 * reduce the CPU to 40 MHz.
 *
 * ESP32-P4 validates the maximum against the frequency selected at build time.
 * Using a hard-coded 240 MHz with a 360 MHz sdkconfig caused esp_pm_configure()
 * to fail, which left every AI lock request in ESP_ERR_INVALID_STATE.
 */
#define CPU_MIN_FREQ_MHZ 40
#define CPU_MAX_FREQ_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ

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
     *
     * The application's existing inactivity policy enters Light-sleep
     * explicitly through esp_light_sleep_start().
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
            "esp_pm_configure failed for min=%d MHz max=%d MHz: %s",
            CPU_MIN_FREQ_MHZ,
            CPU_MAX_FREQ_MHZ,
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * Request the configured build-time maximum only while AI inference runs.
     * Detection and recognition must release this lock on every successful
     * acquisition so DFS can return to the 40 MHz minimum afterward.
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
        "Dynamic CPU scaling configured: min=%d MHz, AI max=%d MHz",
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
