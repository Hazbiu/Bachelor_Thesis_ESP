#include "platform/power/cpu_power.h"

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

static const char *TAG = "PWR_CPU";

typedef enum {
    CPU_POWER_STATE_ACTIVE = 0,
    CPU_POWER_STATE_ECO_SCAN_8,
    CPU_POWER_STATE_ECO_SCAN_16,
} cpu_power_state_t;

static esp_pm_lock_handle_t s_ai_cpu_lock;
static SemaphoreHandle_t s_policy_mutex;
static bool s_initialized;
static bool s_face_boost_active;
static cpu_power_state_t s_state = CPU_POWER_STATE_ACTIVE;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *state_name(cpu_power_state_t state)
{
    if (state == CPU_POWER_STATE_ECO_SCAN_8) {
        return "ECO-SCAN-8";
    }
    if (state == CPU_POWER_STATE_ECO_SCAN_16) {
        return "ECO-SCAN-16";
    }
    return "ACTIVE";
}

static uint32_t detection_interval_for_state(cpu_power_state_t state)
{
    if (state == CPU_POWER_STATE_ECO_SCAN_16) {
        return APP_FACE_DETECT_IDLE_90_INTERVAL_FRAMES;
    }
    if (state == CPU_POWER_STATE_ECO_SCAN_8) {
        return APP_FACE_DETECT_IDLE_180_INTERVAL_FRAMES;
    }
    return APP_FACE_DETECT_INTERVAL_FRAMES;
}

/* Must be called while s_policy_mutex is held. */
static esp_err_t apply_frequency_policy_locked(
    int baseline_mhz,
    cpu_power_state_t state,
    uint32_t inactive_ms)
{
    const esp_pm_config_t requested = {
        .max_freq_mhz = APP_CPU_MAX_FREQ_MHZ,
        .min_freq_mhz = baseline_mhz,
        .light_sleep_enable = false,
    };

    esp_err_t ret = esp_pm_configure(&requested);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "event=CPU_POLICY_REJECTED state=%s baseline_mhz=%d max_mhz=%d "
            "error=%s",
            state_name(state),
            baseline_mhz,
            APP_CPU_MAX_FREQ_MHZ,
            esp_err_to_name(ret));
        return ret;
    }

    esp_pm_config_t applied = {0};
    ret = esp_pm_get_configuration(&applied);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "event=CPU_CLOCK_VERIFY_READ_FAILED error=%s",
                 esp_err_to_name(ret));
        return ret;
    }

    if (applied.min_freq_mhz != baseline_mhz ||
        applied.max_freq_mhz != APP_CPU_MAX_FREQ_MHZ ||
        applied.light_sleep_enable) {
        ESP_LOGE(
            TAG,
            "event=CPU_POLICY_VERIFY_MISMATCH state=%s "
            "requested_baseline_mhz=%d requested_max_mhz=%d "
            "applied_min_mhz=%d applied_max_mhz=%d auto_light_sleep=%s",
            state_name(state),
            baseline_mhz,
            APP_CPU_MAX_FREQ_MHZ,
            applied.min_freq_mhz,
            applied.max_freq_mhz,
            applied.light_sleep_enable ? "on" : "off");
        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "event=CPU_POLICY_APPLIED state=%s inactive_ms=%" PRIu32
        " baseline_mhz=%d max_mhz=%d face_boost=%s "
        "detect_every_frames=%" PRIu32,
        state_name(state),
        inactive_ms,
        baseline_mhz,
        APP_CPU_MAX_FREQ_MHZ,
        s_face_boost_active ? "on" : "off",
        detection_interval_for_state(state));

    return ESP_OK;
}

esp_err_t cpu_power_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ != APP_CPU_MAX_FREQ_MHZ) {
        ESP_LOGE(
            TAG,
            "event=CPU_DEFAULT_MISMATCH expected_mhz=%d configured_mhz=%d",
            APP_CPU_MAX_FREQ_MHZ,
            CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
        return ESP_ERR_INVALID_STATE;
    }

    s_policy_mutex = xSemaphoreCreateMutex();
    if (s_policy_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(s_policy_mutex);
        s_policy_mutex = NULL;
        return ESP_FAIL;
    }

    esp_err_t ret = apply_frequency_policy_locked(
        APP_CPU_ACTIVE_FREQ_MHZ,
        CPU_POWER_STATE_ACTIVE,
        0U);

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
        vSemaphoreDelete(s_policy_mutex);
        s_policy_mutex = NULL;
        return ret;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_state = CPU_POWER_STATE_ACTIVE;
    s_face_boost_active = false;
    s_initialized = true;
    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(
        TAG,
        "PWR-OPT-3 event=CPU_POLICY active_baseline_mhz=%d max_mhz=%d "
        "eco_scan_8_after_ms=%u eco_scan_16_after_ms=%u "
        "camera_display_teardown=disabled",
        APP_CPU_ACTIVE_FREQ_MHZ,
        APP_CPU_MAX_FREQ_MHZ,
        (unsigned)APP_CPU_IDLE_180_AFTER_MS,
        (unsigned)APP_CPU_IDLE_90_AFTER_MS);

    ESP_LOGI(TAG, "PWR-OPT-3: profile=%d backlight_pct=%d/%d/%d "
             "ai_min_ms=%u/%u/%u preview_min_ms=%u/%u/%u",
             APP_POWER_OPTIMIZATION_PROFILE,
             APP_BACKLIGHT_ACTIVE_PERCENT, APP_BACKLIGHT_ECO1_PERCENT,
             APP_BACKLIGHT_ECO2_PERCENT,
             (unsigned)APP_AI_ACTIVE_MIN_INTERVAL_MS,
             (unsigned)APP_AI_ECO1_MIN_INTERVAL_MS,
             (unsigned)APP_AI_ECO2_MIN_INTERVAL_MS,
             (unsigned)APP_PREVIEW_ACTIVE_MIN_INTERVAL_MS,
             (unsigned)APP_PREVIEW_ECO1_MIN_INTERVAL_MS,
             (unsigned)APP_PREVIEW_ECO2_MIN_INTERVAL_MS);

    return ESP_OK;
}

esp_err_t cpu_power_register_idle_scan_callbacks(
    cpu_power_idle_transition_callback_t enter_idle_scan,
    cpu_power_idle_transition_callback_t exit_idle_scan,
    void *user_data)
{
    if (enter_idle_scan == NULL || exit_idle_scan == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Retain the API so app_main does not need a risky broad rewrite. The old
     * callbacks dismantled MIPI-DSI/DPHY while CSI/ISP was still producing
     * interrupts. They are therefore deliberately accepted but not stored or
     * invoked. The coordinated Light-sleep path remains responsible for first
     * stopping the camera and only then suspending the display.
     */
    (void)enter_idle_scan;
    (void)exit_idle_scan;
    (void)user_data;

    ESP_LOGW(
        TAG,
        "event=IDLE_DISPLAY_CALLBACKS_DISABLED reason=CSI_ISP_ACTIVE");
    return ESP_OK;
}

esp_err_t cpu_power_face_boost_begin(void)
{
    if (!s_initialized || s_ai_cpu_lock == NULL || s_policy_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    bool already_active;
    portENTER_CRITICAL(&s_state_lock);
    already_active = s_face_boost_active;
    portEXIT_CRITICAL(&s_state_lock);

    if (already_active) {
        xSemaphoreGive(s_policy_mutex);
        return ESP_OK;
    }

    esp_err_t ret = esp_pm_lock_acquire(s_ai_cpu_lock);

    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_face_boost_active = true;
        portEXIT_CRITICAL(&s_state_lock);
    }

    xSemaphoreGive(s_policy_mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "event=FACE_BOOST_ACQUIRE_FAILED error=%s",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "event=FACE_BOOST_ACQUIRED target_mhz=%d",
             APP_CPU_MAX_FREQ_MHZ);
    return ESP_OK;
}

esp_err_t cpu_power_face_boost_end(void)
{
    if (!s_initialized || s_ai_cpu_lock == NULL || s_policy_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    bool boost_active;
    portENTER_CRITICAL(&s_state_lock);
    boost_active = s_face_boost_active;
    portEXIT_CRITICAL(&s_state_lock);

    if (!boost_active) {
        xSemaphoreGive(s_policy_mutex);
        return ESP_OK;
    }

    esp_err_t ret = esp_pm_lock_release(s_ai_cpu_lock);

    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_face_boost_active = false;
        portEXIT_CRITICAL(&s_state_lock);
    }

    xSemaphoreGive(s_policy_mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "event=FACE_BOOST_RELEASE_FAILED error=%s",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "event=FACE_BOOST_RELEASED active_baseline_mhz=%d",
             APP_CPU_ACTIVE_FREQ_MHZ);
    return ret;
}

bool cpu_power_is_face_boost_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_state_lock);
    active = s_face_boost_active;
    portEXIT_CRITICAL(&s_state_lock);
    return active;
}

static void select_scan_state(
    cpu_power_state_t requested_state,
    uint32_t inactive_ms)
{
    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    /* A no-face detector also uses the boost. Frequency-lock ownership is
     * separate from inactivity; only actual activity restores the fast scan. */
    const cpu_power_state_t target_state = requested_state;

    cpu_power_state_t previous_state;
    portENTER_CRITICAL(&s_state_lock);
    previous_state = s_state;
    s_state = target_state;
    portEXIT_CRITICAL(&s_state_lock);

    if (previous_state != target_state) {
        ESP_LOGI(
            TAG,
            "event=SCAN_RATE_APPLIED state=%s inactive_ms=%" PRIu32
            " baseline_mhz=%d max_mhz=%d detect_every_frames=%" PRIu32
            " camera=on display=on",
            state_name(target_state),
            inactive_ms,
            APP_CPU_ACTIVE_FREQ_MHZ,
            APP_CPU_MAX_FREQ_MHZ,
            detection_interval_for_state(target_state));
    }

    xSemaphoreGive(s_policy_mutex);
}

static void restore_active(void)
{
    select_scan_state(CPU_POWER_STATE_ACTIVE, 0U);
}

void cpu_power_update_inactivity(uint32_t inactive_ms)
{
    if (!s_initialized) {
        return;
    }

    if (inactive_ms >= APP_CPU_IDLE_90_AFTER_MS) {
        select_scan_state(CPU_POWER_STATE_ECO_SCAN_16, inactive_ms);
    } else if (inactive_ms >= APP_CPU_IDLE_180_AFTER_MS) {
        select_scan_state(CPU_POWER_STATE_ECO_SCAN_8, inactive_ms);
    } else {
        restore_active();
    }
}

void cpu_power_notify_activity(void)
{
    if (s_initialized) {
        restore_active();
    }
}

uint32_t cpu_power_get_face_detect_interval_frames(void)
{
    cpu_power_state_t state;
    portENTER_CRITICAL(&s_state_lock);
    state = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    return detection_interval_for_state(state);
}

bool cpu_power_is_idle_scan_active(void)
{
    return false;
}


static cpu_power_state_t current_scan_state(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const cpu_power_state_t state = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}


uint32_t cpu_power_get_ai_min_interval_ms(void)
{
    switch (current_scan_state()) {
    case CPU_POWER_STATE_ECO_SCAN_8: return APP_AI_ECO1_MIN_INTERVAL_MS;
    case CPU_POWER_STATE_ECO_SCAN_16: return APP_AI_ECO2_MIN_INTERVAL_MS;
    default: return APP_AI_ACTIVE_MIN_INTERVAL_MS;
    }
}


uint32_t cpu_power_get_preview_min_interval_ms(void)
{
    switch (current_scan_state()) {
    case CPU_POWER_STATE_ECO_SCAN_8: return APP_PREVIEW_ECO1_MIN_INTERVAL_MS;
    case CPU_POWER_STATE_ECO_SCAN_16: return APP_PREVIEW_ECO2_MIN_INTERVAL_MS;
    default: return APP_PREVIEW_ACTIVE_MIN_INTERVAL_MS;
    }
}


int cpu_power_get_backlight_percent(void)
{
    switch (current_scan_state()) {
    case CPU_POWER_STATE_ECO_SCAN_8: return APP_BACKLIGHT_ECO1_PERCENT;
    case CPU_POWER_STATE_ECO_SCAN_16: return APP_BACKLIGHT_ECO2_PERCENT;
    default: return APP_BACKLIGHT_ACTIVE_PERCENT;
    }
}
