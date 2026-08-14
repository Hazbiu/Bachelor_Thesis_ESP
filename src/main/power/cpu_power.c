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

static const char *TAG = "PWR_CPU";

typedef enum {
    CPU_POWER_STATE_ACTIVE = 0,
    CPU_POWER_STATE_ENTERING_IDLE_SCAN,
    CPU_POWER_STATE_IDLE_SCAN_180,
    CPU_POWER_STATE_IDLE_SCAN_90,
} cpu_power_state_t;

static esp_pm_lock_handle_t s_ai_cpu_lock;
static SemaphoreHandle_t s_policy_mutex;
static bool s_initialized;
static bool s_ai_session_active;
static bool s_idle_90_rejected_this_window;
static cpu_power_state_t s_state = CPU_POWER_STATE_ACTIVE;

static cpu_power_idle_transition_callback_t s_enter_idle_scan;
static cpu_power_idle_transition_callback_t s_exit_idle_scan;
static void *s_transition_user_data;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *state_name(cpu_power_state_t state)
{
    if (state == CPU_POWER_STATE_IDLE_SCAN_180) {
        return "IDLE-SCAN-180";
    }
    if (state == CPU_POWER_STATE_IDLE_SCAN_90) {
        return "IDLE-SCAN-90";
    }
    if (state == CPU_POWER_STATE_ENTERING_IDLE_SCAN) {
        return "ENTERING-IDLE-SCAN";
    }
    return "ACTIVE";
}

static uint32_t detection_interval_for_state(cpu_power_state_t state)
{
    if (state == CPU_POWER_STATE_IDLE_SCAN_90) {
        return APP_FACE_DETECT_IDLE_90_INTERVAL_FRAMES;
    }
    if (state == CPU_POWER_STATE_IDLE_SCAN_180 ||
        state == CPU_POWER_STATE_ENTERING_IDLE_SCAN) {
        return APP_FACE_DETECT_IDLE_180_INTERVAL_FRAMES;
    }
    return APP_FACE_DETECT_INTERVAL_FRAMES;
}

/* Must be called while s_policy_mutex is held. */
static esp_err_t apply_fixed_frequency_locked(
    int frequency_mhz,
    cpu_power_state_t state,
    uint32_t inactive_ms)
{
    const esp_pm_config_t requested = {
        .max_freq_mhz = frequency_mhz,
        .min_freq_mhz = frequency_mhz,
        .light_sleep_enable = false,
    };

    esp_err_t ret = esp_pm_configure(&requested);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "event=CPU_CLOCK_REJECTED state=%s requested_mhz=%d error=%s",
            state_name(state),
            frequency_mhz,
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

    if (applied.min_freq_mhz != frequency_mhz ||
        applied.max_freq_mhz != frequency_mhz ||
        applied.light_sleep_enable) {
        ESP_LOGE(
            TAG,
            "event=CPU_CLOCK_VERIFY_MISMATCH state=%s requested_mhz=%d "
            "applied_min_mhz=%d applied_max_mhz=%d auto_light_sleep=%s",
            state_name(state),
            frequency_mhz,
            applied.min_freq_mhz,
            applied.max_freq_mhz,
            applied.light_sleep_enable ? "on" : "off");
        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "event=CPU_CLOCK_APPLIED state=%s inactive_ms=%" PRIu32
        " cpu_mhz=%d detect_every_frames=%" PRIu32,
        state_name(state),
        inactive_ms,
        frequency_mhz,
        detection_interval_for_state(state));

    return ESP_OK;
}

esp_err_t cpu_power_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ != APP_CPU_ACTIVE_FREQ_MHZ) {
        ESP_LOGE(
            TAG,
            "event=CPU_DEFAULT_MISMATCH expected_mhz=%d configured_mhz=%d",
            APP_CPU_ACTIVE_FREQ_MHZ,
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

    esp_err_t ret = apply_fixed_frequency_locked(
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
    s_ai_session_active = false;
    s_idle_90_rejected_this_window = false;
    s_initialized = true;
    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(
        TAG,
        "event=CPU_POLICY active_mhz=%d idle_180_mhz=%d "
        "idle_180_after_ms=%u idle_90_mhz=%d idle_90_after_ms=%u",
        APP_CPU_ACTIVE_FREQ_MHZ,
        APP_CPU_IDLE_180_FREQ_MHZ,
        (unsigned)APP_CPU_IDLE_180_AFTER_MS,
        APP_CPU_IDLE_90_FREQ_MHZ,
        (unsigned)APP_CPU_IDLE_90_AFTER_MS);

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

    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    s_enter_idle_scan = enter_idle_scan;
    s_exit_idle_scan = exit_idle_scan;
    s_transition_user_data = user_data;

    xSemaphoreGive(s_policy_mutex);
    return ESP_OK;
}

esp_err_t cpu_power_ai_begin(void)
{
    if (!s_initialized || s_ai_cpu_lock == NULL || s_policy_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    esp_err_t ret = esp_pm_lock_acquire(s_ai_cpu_lock);
    if (ret != ESP_OK) {
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
    if (!s_initialized || s_ai_cpu_lock == NULL || s_policy_mutex == NULL) {
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

    portENTER_CRITICAL(&s_state_lock);
    s_ai_session_active = false;
    portEXIT_CRITICAL(&s_state_lock);

    xSemaphoreGive(s_policy_mutex);
    return ret;
}

static void enter_idle_scan(uint32_t inactive_ms)
{
    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    if (s_state != CPU_POWER_STATE_ACTIVE) {
        xSemaphoreGive(s_policy_mutex);
        return;
    }

    if (s_enter_idle_scan == NULL || s_exit_idle_scan == NULL) {
        ESP_LOGW(TAG, "event=IDLE_CALLBACKS_MISSING");
        xSemaphoreGive(s_policy_mutex);
        return;
    }

    /*
     * Mark the transition, then release the CPU-policy mutex before taking the
     * application's display mutex. The video callback takes these locks in the
     * opposite order, so holding both here would create a lock inversion.
     */
    portENTER_CRITICAL(&s_state_lock);
    s_state = CPU_POWER_STATE_ENTERING_IDLE_SCAN;
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_policy_mutex);

    /* The application removes MIPI-DSI while the CPU is still at 360 MHz. */
    esp_err_t ret = s_enter_idle_scan(s_transition_user_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "event=IDLE_DISPLAY_SUSPEND_FAILED error=%s",
                 esp_err_to_name(ret));

        if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) == pdTRUE) {
            portENTER_CRITICAL(&s_state_lock);
            s_state = CPU_POWER_STATE_ACTIVE;
            portEXIT_CRITICAL(&s_state_lock);
            xSemaphoreGive(s_policy_mutex);
        }
        (void)s_exit_idle_scan(s_transition_user_data);
        return;
    }

    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        (void)s_exit_idle_scan(s_transition_user_data);
        return;
    }

    /* Activity may have cancelled the transition after display suspension. */
    if (s_state != CPU_POWER_STATE_ENTERING_IDLE_SCAN) {
        xSemaphoreGive(s_policy_mutex);
        return;
    }

    ret = apply_fixed_frequency_locked(
        APP_CPU_IDLE_180_FREQ_MHZ,
        CPU_POWER_STATE_IDLE_SCAN_180,
        inactive_ms);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "event=IDLE_180_FAILED action=RESTORE_ACTIVE");
        (void)apply_fixed_frequency_locked(
            APP_CPU_ACTIVE_FREQ_MHZ,
            CPU_POWER_STATE_ACTIVE,
            inactive_ms);
        portENTER_CRITICAL(&s_state_lock);
        s_state = CPU_POWER_STATE_ACTIVE;
        portEXIT_CRITICAL(&s_state_lock);
        (void)s_exit_idle_scan(s_transition_user_data);
        xSemaphoreGive(s_policy_mutex);
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_state = CPU_POWER_STATE_IDLE_SCAN_180;
    portEXIT_CRITICAL(&s_state_lock);
    s_idle_90_rejected_this_window = false;

    xSemaphoreGive(s_policy_mutex);
}

static void enter_idle_scan_90(uint32_t inactive_ms)
{
    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    if (s_state == CPU_POWER_STATE_IDLE_SCAN_90 ||
        s_idle_90_rejected_this_window) {
        xSemaphoreGive(s_policy_mutex);
        return;
    }

    /*
     * The 90 MHz stage is legal only after the display has been removed and
     * the first 180 MHz stage is active. If 90 MHz is rejected, explicitly
     * reapply 180 MHz and continue scanning instead of failing the system.
     */
    if (s_state != CPU_POWER_STATE_IDLE_SCAN_180) {
        xSemaphoreGive(s_policy_mutex);
        return;
    }

    esp_err_t ret = apply_fixed_frequency_locked(
        APP_CPU_IDLE_90_FREQ_MHZ,
        CPU_POWER_STATE_IDLE_SCAN_90,
        inactive_ms);

    if (ret != ESP_OK) {
        s_idle_90_rejected_this_window = true;
        ESP_LOGW(
            TAG,
            "event=CPU_CLOCK_FALLBACK failed_mhz=%d fallback_mhz=%d",
            APP_CPU_IDLE_90_FREQ_MHZ,
            APP_CPU_IDLE_180_FREQ_MHZ);
        (void)apply_fixed_frequency_locked(
            APP_CPU_IDLE_180_FREQ_MHZ,
            CPU_POWER_STATE_IDLE_SCAN_180,
            inactive_ms);
        xSemaphoreGive(s_policy_mutex);
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_state = CPU_POWER_STATE_IDLE_SCAN_90;
    portEXIT_CRITICAL(&s_state_lock);

    xSemaphoreGive(s_policy_mutex);
}

static void restore_active(void)
{
    if (xSemaphoreTake(s_policy_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    const cpu_power_state_t previous_state = s_state;

    if (previous_state == CPU_POWER_STATE_ACTIVE) {
        xSemaphoreGive(s_policy_mutex);
        return;
    }

    esp_err_t ret = ESP_OK;

    if (previous_state == CPU_POWER_STATE_IDLE_SCAN_180 ||
        previous_state == CPU_POWER_STATE_IDLE_SCAN_90) {
        /* Restore 360 MHz before asking the application to start MIPI-DSI. */
        ret = apply_fixed_frequency_locked(
            APP_CPU_ACTIVE_FREQ_MHZ,
            CPU_POWER_STATE_ACTIVE,
            0U);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "event=ACTIVE_RESTORE_FAILED requested_mhz=360");
        xSemaphoreGive(s_policy_mutex);
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_state = CPU_POWER_STATE_ACTIVE;
    portEXIT_CRITICAL(&s_state_lock);
    s_idle_90_rejected_this_window = false;

    /* This callback only schedules display restoration; it must not block. */
    if (s_exit_idle_scan != NULL) {
        ret = s_exit_idle_scan(s_transition_user_data);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "event=ACTIVE_DISPLAY_RESTORE_FAILED error=%s",
                     esp_err_to_name(ret));
        }
    }

    xSemaphoreGive(s_policy_mutex);
}

void cpu_power_update_inactivity(uint32_t inactive_ms)
{
    if (!s_initialized) {
        return;
    }

    if (inactive_ms >= APP_CPU_IDLE_90_AFTER_MS) {
        enter_idle_scan(inactive_ms);
        enter_idle_scan_90(inactive_ms);
    } else if (inactive_ms >= APP_CPU_IDLE_180_AFTER_MS) {
        enter_idle_scan(inactive_ms);
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
    bool idle;
    portENTER_CRITICAL(&s_state_lock);
    idle = s_state != CPU_POWER_STATE_ACTIVE;
    portEXIT_CRITICAL(&s_state_lock);
    return idle;
}
