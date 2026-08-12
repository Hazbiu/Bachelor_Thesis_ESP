#include "power_save/cpu_power.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config/app_config.h"

static const char *TAG = "cpu_power";

/*
 * ESP32-P4 active CPU policy.
 *
 * The complete active window keeps the display-safe 90..360 MHz DFS range.
 * ESP-IDF may therefore raise the CPU/APB clock when the scheduler or a driver
 * needs more bandwidth and return to the 90 MHz floor between work.
 *
 * PERFORMANCE explicitly acquires ESP_PM_CPU_FREQ_MAX during AI inference.
 * BALANCED and ECO do not acquire that application lock and reduce the face
 * detection duty cycle. This saves active power without imposing the unsafe
 * 90 MHz hard maximum that caused MIPI-DSI underruns in the V3 test.
 *
 * Actual Light-sleep is still entered explicitly by app_sleep.c. During
 * Light-sleep, the CPU clock is gated rather than merely reduced to 90 MHz.
 */
#define CPU_ACTIVE_FLOOR_FREQ_MHZ 90
#define CPU_PERFORMANCE_MAX_FREQ_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ

typedef enum {
    CPU_POWER_PROFILE_PERFORMANCE = 0,
    CPU_POWER_PROFILE_BALANCED,
    CPU_POWER_PROFILE_ECO,
} cpu_power_profile_t;

static esp_pm_lock_handle_t s_ai_cpu_lock;
static SemaphoreHandle_t s_policy_mutex;
static bool s_initialized;
static bool s_ai_session_holds_mutex;
static cpu_power_profile_t s_profile = CPU_POWER_PROFILE_PERFORMANCE;
static int s_applied_min_mhz;
static int s_applied_max_mhz;

/* Used only for short cross-task reads/writes of the profile/session flags. */
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *profile_name(cpu_power_profile_t profile)
{
    switch (profile) {
    case CPU_POWER_PROFILE_PERFORMANCE:
        return "PERFORMANCE";
    case CPU_POWER_PROFILE_BALANCED:
        return "BALANCED";
    case CPU_POWER_PROFILE_ECO:
        return "ECO";
    default:
        return "UNKNOWN";
    }
}

static uint32_t profile_detection_interval(cpu_power_profile_t profile)
{
    switch (profile) {
    case CPU_POWER_PROFILE_BALANCED:
        return APP_FACE_DETECT_BALANCED_INTERVAL_FRAMES;
    case CPU_POWER_PROFILE_ECO:
        return APP_FACE_DETECT_ECO_INTERVAL_FRAMES;
    case CPU_POWER_PROFILE_PERFORMANCE:
    default:
        return APP_FACE_DETECT_INTERVAL_FRAMES;
    }
}

/*
 * Must be called while s_policy_mutex is held.
 *
 * esp_pm_get_configuration() verifies that ESP-IDF accepted exactly the
 * requested range. The log therefore reports the applied policy, not only the
 * requested one.
 */
static esp_err_t apply_active_dfs_policy_locked(void)
{
    const esp_pm_config_t requested = {
        .max_freq_mhz = CPU_PERFORMANCE_MAX_FREQ_MHZ,
        .min_freq_mhz = CPU_ACTIVE_FLOOR_FREQ_MHZ,
        .light_sleep_enable = false,
    };

    esp_err_t ret = esp_pm_configure(&requested);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "ACTIVE-POWER-V6 esp_pm_configure rejected %d..%d MHz: %s",
            CPU_ACTIVE_FLOOR_FREQ_MHZ,
            CPU_PERFORMANCE_MAX_FREQ_MHZ,
            esp_err_to_name(ret));
        return ret;
    }

    esp_pm_config_t applied = {0};
    ret = esp_pm_get_configuration(&applied);
    if (ret != ESP_OK) {
        s_applied_min_mhz = 0;
        s_applied_max_mhz = 0;
        ESP_LOGE(
            TAG,
            "ACTIVE-POWER-V6 could not verify PM configuration: %s",
            esp_err_to_name(ret));
        return ret;
    }

    s_applied_min_mhz = applied.min_freq_mhz;
    s_applied_max_mhz = applied.max_freq_mhz;

    if (applied.min_freq_mhz != CPU_ACTIVE_FLOOR_FREQ_MHZ ||
        applied.max_freq_mhz != CPU_PERFORMANCE_MAX_FREQ_MHZ ||
        applied.light_sleep_enable) {
        ESP_LOGE(
            TAG,
            "ACTIVE-POWER-V6 PM verification mismatch: requested=%d..%d "
            "applied=%d..%d MHz auto_light_sleep=%s",
            CPU_ACTIVE_FLOOR_FREQ_MHZ,
            CPU_PERFORMANCE_MAX_FREQ_MHZ,
            applied.min_freq_mhz,
            applied.max_freq_mhz,
            applied.light_sleep_enable ? "on" : "off");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void log_profile(cpu_power_profile_t profile)
{
    const bool ai_max_lock_enabled =
        profile == CPU_POWER_PROFILE_PERFORMANCE;

    ESP_LOGI(
        TAG,
        "ACTIVE-POWER-V6 profile=%s active_DFS=%d..%d MHz "
        "AI_max_lock=%s policy=verified detect_every=%" PRIu32 " frames",
        profile_name(profile),
        s_applied_min_mhz,
        s_applied_max_mhz,
        ai_max_lock_enabled ? "enabled" : "disabled",
        profile_detection_interval(profile));
}

static void set_profile(cpu_power_profile_t new_profile)
{
    if (!s_initialized || s_policy_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "ACTIVE-POWER-V6 failed to lock CPU policy mutex");
        return;
    }

    cpu_power_profile_t old_profile;
    portENTER_CRITICAL(&s_state_lock);
    old_profile = s_profile;
    portEXIT_CRITICAL(&s_state_lock);

    if (old_profile == new_profile) {
        xSemaphoreGive(s_policy_mutex);
        return;
    }

    /* Only the AI-lock policy and detection interval change here. */
    portENTER_CRITICAL(&s_state_lock);
    s_profile = new_profile;
    portEXIT_CRITICAL(&s_state_lock);

    log_profile(new_profile);
    xSemaphoreGive(s_policy_mutex);
}

esp_err_t cpu_power_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_policy_mutex = xSemaphoreCreateMutex();
    if (s_policy_mutex == NULL) {
        ESP_LOGE(TAG, "Creating CPU policy mutex failed");
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(s_policy_mutex);
        s_policy_mutex = NULL;
        return ESP_FAIL;
    }

    esp_err_t ret = apply_active_dfs_policy_locked();

    xSemaphoreGive(s_policy_mutex);

    if (ret != ESP_OK) {
        vSemaphoreDelete(s_policy_mutex);
        s_policy_mutex = NULL;
        return ret;
    }

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
        vSemaphoreDelete(s_policy_mutex);
        s_policy_mutex = NULL;
        return ret;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_profile = CPU_POWER_PROFILE_PERFORMANCE;
    s_ai_session_holds_mutex = false;
    s_initialized = true;
    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(
        TAG,
        "ACTIVE-POWER-V6 initialized: active_DFS=%d..%d MHz, "
        "BALANCED/ECO release AI max lock, balanced=%d ms, eco=%d ms",
        CPU_ACTIVE_FLOOR_FREQ_MHZ,
        CPU_PERFORMANCE_MAX_FREQ_MHZ,
        APP_CPU_BALANCED_AFTER_MS,
        APP_CPU_ECO_AFTER_MS);
    log_profile(CPU_POWER_PROFILE_PERFORMANCE);

    return ESP_OK;
}

esp_err_t cpu_power_ai_begin(void)
{
    if (!s_initialized || s_ai_cpu_lock == NULL ||
        s_policy_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Hold the policy mutex until cpu_power_ai_end() in PERFORMANCE mode.
     * This prevents the inactivity task from changing the logical profile
     * while the same AI operation owns its maximum-frequency lock.
     */
    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    cpu_power_profile_t profile;
    portENTER_CRITICAL(&s_state_lock);
    profile = s_profile;
    portEXIT_CRITICAL(&s_state_lock);

    if (profile != CPU_POWER_PROFILE_PERFORMANCE) {
        xSemaphoreGive(s_policy_mutex);
        return ESP_OK;
    }

    esp_err_t ret = esp_pm_lock_acquire(s_ai_cpu_lock);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Acquiring AI CPU lock failed: %s",
            esp_err_to_name(ret));
        xSemaphoreGive(s_policy_mutex);
        return ret;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_ai_session_holds_mutex = true;
    portEXIT_CRITICAL(&s_state_lock);

    return ESP_OK;
}

esp_err_t cpu_power_ai_end(void)
{
    if (!s_initialized || s_ai_cpu_lock == NULL ||
        s_policy_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool release_required;
    portENTER_CRITICAL(&s_state_lock);
    release_required = s_ai_session_holds_mutex;
    portEXIT_CRITICAL(&s_state_lock);

    /* BALANCED/ECO cpu_power_ai_begin() is intentionally a successful no-op. */
    if (!release_required) {
        return ESP_OK;
    }

    esp_err_t ret = esp_pm_lock_release(s_ai_cpu_lock);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Releasing AI CPU lock failed: %s",
            esp_err_to_name(ret));
    }

    portENTER_CRITICAL(&s_state_lock);
    s_ai_session_holds_mutex = false;
    portEXIT_CRITICAL(&s_state_lock);

    /* Always release the mutex so a PM-lock error cannot deadlock the system. */
    xSemaphoreGive(s_policy_mutex);
    return ret;
}

void cpu_power_update_inactivity(uint32_t inactive_ms)
{
    if (!s_initialized) {
        return;
    }

    cpu_power_profile_t new_profile = CPU_POWER_PROFILE_PERFORMANCE;

    if (inactive_ms >= APP_CPU_ECO_AFTER_MS) {
        new_profile = CPU_POWER_PROFILE_ECO;
    } else if (inactive_ms >= APP_CPU_BALANCED_AFTER_MS) {
        new_profile = CPU_POWER_PROFILE_BALANCED;
    }

    set_profile(new_profile);
}

void cpu_power_notify_activity(void)
{
    if (s_initialized) {
        set_profile(CPU_POWER_PROFILE_PERFORMANCE);
    }
}

uint32_t cpu_power_get_face_detect_interval_frames(void)
{
    cpu_power_profile_t profile;

    portENTER_CRITICAL(&s_state_lock);
    profile = s_profile;
    portEXIT_CRITICAL(&s_state_lock);

    const uint32_t interval = profile_detection_interval(profile);
    return interval > 0 ? interval : 1;
}
