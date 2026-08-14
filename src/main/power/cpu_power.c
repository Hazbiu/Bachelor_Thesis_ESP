#include "power_save/cpu_power.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config/app_config.h"

static const char *TAG = "cpu_power";

typedef struct {
    uint32_t after_ms;
    int frequency_mhz;
    const char *name;
} cpu_sweep_stage_t;

/*
 * Experimental frequency candidates.
 *
 * esp_pm_configure() remains the authority for whether a requested frequency
 * is supported by the current ESP-IDF version and ESP32-P4 revision.
 *
 * If a stage is rejected, the previous accepted configuration is restored.
 */
static const cpu_sweep_stage_t s_sweep_stages[] = {
    {
        .after_ms = 0U,
        .frequency_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .name = "FULL"
    },
    {
        .after_ms = APP_CPU_SWEEP_STAGE_240_AFTER_MS,
        .frequency_mhz = 240,
        .name = "STEP-240"
    },
    {
        .after_ms = APP_CPU_SWEEP_STAGE_180_AFTER_MS,
        .frequency_mhz = 180,
        .name = "STEP-180"
    },
    {
        .after_ms = APP_CPU_SWEEP_STAGE_120_AFTER_MS,
        .frequency_mhz = 120,
        .name = "STEP-120"
    },
    {
        .after_ms = APP_CPU_SWEEP_STAGE_90_AFTER_MS,
        .frequency_mhz = 90,
        .name = "STEP-90"
    },
    {
        .after_ms = APP_CPU_SWEEP_STAGE_60_AFTER_MS,
        .frequency_mhz = 60,
        .name = "STEP-60"
    },
    {
        .after_ms = APP_CPU_SWEEP_STAGE_40_AFTER_MS,
        .frequency_mhz = 40,
        .name = "STEP-40"
    }
};

#define CPU_SWEEP_STAGE_COUNT \
    (sizeof(s_sweep_stages) / sizeof(s_sweep_stages[0]))

static esp_pm_lock_handle_t s_ai_cpu_lock;
static SemaphoreHandle_t s_policy_mutex;

static bool s_initialized;
static bool s_ai_session_active;

/*
 * requested_stage records the most recent attempted stage. This prevents an
 * unsupported stage from being submitted every 100 ms by the inactivity task.
 */
static size_t s_requested_stage;

/*
 * accepted_stage records the most recent stage successfully accepted and
 * verified by ESP-IDF.
 */
static size_t s_accepted_stage;

static int s_applied_min_mhz;
static int s_applied_max_mhz;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t read_applied_configuration(esp_pm_config_t *configuration)
{
    if (configuration == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = esp_pm_get_configuration(configuration);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "CPU-SWEEP could not read PM configuration: %s",
            esp_err_to_name(ret));
    }

    return ret;
}

/*
 * Must be called while s_policy_mutex is held.
 */
static esp_err_t restore_previous_policy_locked(
    int previous_min_mhz,
    int previous_max_mhz)
{
    if (previous_min_mhz <= 0 || previous_max_mhz <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_pm_config_t restore_configuration = {
        .max_freq_mhz = previous_max_mhz,
        .min_freq_mhz = previous_min_mhz,
        .light_sleep_enable = false,
    };

    esp_err_t ret = esp_pm_configure(&restore_configuration);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "CPU-SWEEP CRITICAL: restoring %d..%d MHz failed: %s",
            previous_min_mhz,
            previous_max_mhz,
            esp_err_to_name(ret));

        return ret;
    }

    esp_pm_config_t applied = {0};
    ret = read_applied_configuration(&applied);

    if (ret != ESP_OK) {
        return ret;
    }

    s_applied_min_mhz = applied.min_freq_mhz;
    s_applied_max_mhz = applied.max_freq_mhz;

    ESP_LOGW(
        TAG,
        "CPU-SWEEP restored last accepted policy=%d..%d MHz",
        s_applied_min_mhz,
        s_applied_max_mhz);

    return ESP_OK;
}

/*
 * Apply one fixed-frequency stage.
 *
 * Setting min_freq_mhz and max_freq_mhz to the same value prevents DFS from
 * choosing another CPU frequency between AI operations.
 *
 * Must be called while s_policy_mutex is held.
 */
static esp_err_t apply_fixed_frequency_locked(
    size_t stage_index,
    uint32_t inactive_ms)
{
    if (stage_index >= CPU_SWEEP_STAGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    const cpu_sweep_stage_t *stage = &s_sweep_stages[stage_index];
    const int previous_min_mhz = s_applied_min_mhz;
    const int previous_max_mhz = s_applied_max_mhz;

    const esp_pm_config_t requested = {
        .max_freq_mhz = stage->frequency_mhz,
        .min_freq_mhz = stage->frequency_mhz,
        .light_sleep_enable = false,
    };

    ESP_LOGW(
        TAG,
        "CPU-SWEEP attempting stage=%u name=%s inactive=%" PRIu32
        " ms requested=%d MHz",
        (unsigned)stage_index,
        stage->name,
        inactive_ms,
        stage->frequency_mhz);

    esp_err_t ret = esp_pm_configure(&requested);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "CPU-SWEEP REJECTED stage=%u name=%s requested=%d MHz "
            "error=%s; last_accepted=%d..%d MHz",
            (unsigned)stage_index,
            stage->name,
            stage->frequency_mhz,
            esp_err_to_name(ret),
            previous_min_mhz,
            previous_max_mhz);

        /*
         * Restore explicitly because the implementation should not assume that
         * a rejected reconfiguration left every PM field untouched.
         */
        esp_err_t restore_ret = restore_previous_policy_locked(
            previous_min_mhz,
            previous_max_mhz);

        if (restore_ret != ESP_OK) {
            return restore_ret;
        }

        return ret;
    }

    esp_pm_config_t applied = {0};
    ret = read_applied_configuration(&applied);

    if (ret != ESP_OK) {
        (void)restore_previous_policy_locked(
            previous_min_mhz,
            previous_max_mhz);

        return ret;
    }

    if (applied.min_freq_mhz != stage->frequency_mhz ||
        applied.max_freq_mhz != stage->frequency_mhz ||
        applied.light_sleep_enable) {
        ESP_LOGE(
            TAG,
            "CPU-SWEEP VERIFICATION-FAILED stage=%u requested=%d MHz "
            "applied=%d..%d MHz auto_light_sleep=%s",
            (unsigned)stage_index,
            stage->frequency_mhz,
            applied.min_freq_mhz,
            applied.max_freq_mhz,
            applied.light_sleep_enable ? "on" : "off");

        (void)restore_previous_policy_locked(
            previous_min_mhz,
            previous_max_mhz);

        return ESP_FAIL;
    }

    s_applied_min_mhz = applied.min_freq_mhz;
    s_applied_max_mhz = applied.max_freq_mhz;
    s_accepted_stage = stage_index;

    ESP_LOGW(
        TAG,
        "CPU-SWEEP APPLIED stage=%u name=%s inactive=%" PRIu32
        " ms fixed_clock=%d MHz detect_every=%u frames",
        (unsigned)stage_index,
        stage->name,
        inactive_ms,
        s_applied_max_mhz,
        (unsigned)APP_FACE_DETECT_INTERVAL_FRAMES);

    return ESP_OK;
}

static size_t select_stage(uint32_t inactive_ms)
{
    size_t selected_stage = 0;

    /*
     * The array is ordered from the earliest to the latest inactivity stage.
     */
    for (size_t index = 1; index < CPU_SWEEP_STAGE_COUNT; index++) {
        if (inactive_ms < s_sweep_stages[index].after_ms) {
            break;
        }

        selected_stage = index;
    }

    return selected_stage;
}

static void request_stage(size_t new_stage, uint32_t inactive_ms)
{
    if (!s_initialized || s_policy_mutex == NULL) {
        return;
    }

    if (new_stage >= CPU_SWEEP_STAGE_COUNT) {
        ESP_LOGE(
            TAG,
            "CPU-SWEEP invalid requested stage=%u",
            (unsigned)new_stage);
        return;
    }

    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "CPU-SWEEP failed to lock CPU policy mutex");
        return;
    }

    size_t old_requested_stage;

    portENTER_CRITICAL(&s_state_lock);
    old_requested_stage = s_requested_stage;
    portEXIT_CRITICAL(&s_state_lock);

    if (old_requested_stage == new_stage) {
        xSemaphoreGive(s_policy_mutex);
        return;
    }

    /*
     * Record the attempted stage even if ESP-IDF rejects it. This prevents
     * repeated attempts during every inactivity-policy poll.
     */
    portENTER_CRITICAL(&s_state_lock);
    s_requested_stage = new_stage;
    portEXIT_CRITICAL(&s_state_lock);

    esp_err_t ret = apply_fixed_frequency_locked(new_stage, inactive_ms);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "CPU-SWEEP stage=%u was not activated; continuing at "
            "last accepted stage=%u fixed_clock=%d MHz",
            (unsigned)new_stage,
            (unsigned)s_accepted_stage,
            s_applied_max_mhz);
    }

    xSemaphoreGive(s_policy_mutex);
}

esp_err_t cpu_power_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ !=
        s_sweep_stages[0].frequency_mhz) {
        ESP_LOGE(
            TAG,
            "CPU-SWEEP invalid initial stage: sdkconfig=%d MHz stage=%d MHz",
            CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            s_sweep_stages[0].frequency_mhz);

        return ESP_ERR_INVALID_STATE;
    }

    s_policy_mutex = xSemaphoreCreateMutex();

    if (s_policy_mutex == NULL) {
        ESP_LOGE(TAG, "CPU-SWEEP creating CPU policy mutex failed");
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(s_policy_mutex);
        s_policy_mutex = NULL;
        return ESP_FAIL;
    }

    /*
     * Initialize these values so restoration is possible if verification of
     * the first configuration fails after esp_pm_configure() succeeds.
     */
    s_applied_min_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    s_applied_max_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    s_requested_stage = 0;
    s_accepted_stage = 0;

    esp_err_t ret = apply_fixed_frequency_locked(0, 0U);

    xSemaphoreGive(s_policy_mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "CPU-SWEEP initial frequency configuration failed: %s",
            esp_err_to_name(ret));

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
            "CPU-SWEEP creating AI CPU lock failed: %s",
            esp_err_to_name(ret));

        vSemaphoreDelete(s_policy_mutex);
        s_policy_mutex = NULL;
        return ret;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_ai_session_active = false;
    s_initialized = true;
    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGW(
        TAG,
        "CPU-SWEEP initialized: stages=360,240,180,120,90,60,40 MHz "
        "stage_time=3000 ms detection_interval=%u frames",
        (unsigned)APP_FACE_DETECT_INTERVAL_FRAMES);

    ESP_LOGW(
        TAG,
        "CPU-SWEEP experimental mode: Light-sleep=%u ms "
        "Deep-sleep=%u ms",
        (unsigned)APP_LIGHT_SLEEP_TIMEOUT_MS,
        (unsigned)APP_DEEP_SLEEP_TIMEOUT_MS);

    return ESP_OK;
}

esp_err_t cpu_power_ai_begin(void)
{
    if (!s_initialized ||
        s_ai_cpu_lock == NULL ||
        s_policy_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Keep the mutex until cpu_power_ai_end(). This guarantees that the
     * inactivity task cannot change frequency in the middle of an inference.
     */
    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    esp_err_t ret = esp_pm_lock_acquire(s_ai_cpu_lock);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "CPU-SWEEP acquiring AI CPU lock failed: %s",
            esp_err_to_name(ret));

        xSemaphoreGive(s_policy_mutex);
        return ret;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_ai_session_active = true;
    portEXIT_CRITICAL(&s_state_lock);

    return ESP_OK;
}

esp_err_t cpu_power_ai_end(void)
{
    if (!s_initialized ||
        s_ai_cpu_lock == NULL ||
        s_policy_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool session_active;

    portENTER_CRITICAL(&s_state_lock);
    session_active = s_ai_session_active;
    portEXIT_CRITICAL(&s_state_lock);

    if (!session_active) {
        return ESP_OK;
    }

    esp_err_t ret = esp_pm_lock_release(s_ai_cpu_lock);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "CPU-SWEEP releasing AI CPU lock failed: %s",
            esp_err_to_name(ret));
    }

    portENTER_CRITICAL(&s_state_lock);
    s_ai_session_active = false;
    portEXIT_CRITICAL(&s_state_lock);

    /*
     * Always release the mutex so a PM-lock error cannot deadlock the
     * frequency policy.
     */
    xSemaphoreGive(s_policy_mutex);

    return ret;
}

void cpu_power_update_inactivity(uint32_t inactive_ms)
{
    if (!s_initialized) {
        return;
    }

    const size_t selected_stage = select_stage(inactive_ms);
    request_stage(selected_stage, inactive_ms);
}

void cpu_power_notify_activity(void)
{
    if (!s_initialized) {
        return;
    }

    /*
     * A face, touchscreen event, PIN event or other accepted activity resets
     * the inactivity timer and immediately requests full performance.
     */
    request_stage(0, 0U);
}

uint32_t cpu_power_get_face_detect_interval_frames(void)
{
    /*
     * Keep the workload constant throughout the experiment. Changing the
     * interval together with CPU frequency would invalidate the comparison.
     */
    return APP_FACE_DETECT_INTERVAL_FRAMES > 0
        ? APP_FACE_DETECT_INTERVAL_FRAMES
        : 1U;
}
