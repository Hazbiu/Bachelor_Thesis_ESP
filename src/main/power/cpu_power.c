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

typedef enum {
    CPU_POWER_STATE_ACTIVE = 0,
    CPU_POWER_STATE_ENTERING_IDLE_SCAN,
    CPU_POWER_STATE_IDLE_SCAN,
} cpu_power_state_t;

static esp_pm_lock_handle_t s_ai_cpu_lock;
static SemaphoreHandle_t s_policy_mutex;
static bool s_initialized;
static bool s_ai_session_active;
static cpu_power_state_t s_state = CPU_POWER_STATE_ACTIVE;

static cpu_power_idle_transition_callback_t s_enter_idle_scan;
static cpu_power_idle_transition_callback_t s_exit_idle_scan;
static void *s_transition_user_data;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *state_name(cpu_power_state_t state)
{
    if (state == CPU_POWER_STATE_IDLE_SCAN) {
        return "IDLE-SCAN";
    }
    if (state == CPU_POWER_STATE_ENTERING_IDLE_SCAN) {
        return "ENTERING-IDLE-SCAN";
    }
    return "ACTIVE";
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
            "ADAPTIVE-POWER rejected state=%s requested=%d MHz: %s",
            state_name(state),
            frequency_mhz,
            esp_err_to_name(ret));
        return ret;
    }

    esp_pm_config_t applied = {0};
    ret = esp_pm_get_configuration(&applied);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADAPTIVE-POWER verification read failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    if (applied.min_freq_mhz != frequency_mhz ||
        applied.max_freq_mhz != frequency_mhz ||
        applied.light_sleep_enable) {
        ESP_LOGE(
            TAG,
            "ADAPTIVE-POWER verification mismatch: state=%s requested=%d "
            "applied=%d..%d MHz auto_light_sleep=%s",
            state_name(state),
            frequency_mhz,
            applied.min_freq_mhz,
            applied.max_freq_mhz,
            applied.light_sleep_enable ? "on" : "off");
        return ESP_FAIL;
    }

    ESP_LOGW(
        TAG,
        "ADAPTIVE-POWER APPLIED state=%s inactive=%" PRIu32
        " ms fixed_clock=%d MHz detect_every=%" PRIu32 " frames",
        state_name(state),
        inactive_ms,
        frequency_mhz,
        state == CPU_POWER_STATE_IDLE_SCAN
            ? (uint32_t)APP_FACE_DETECT_IDLE_INTERVAL_FRAMES
            : (uint32_t)APP_FACE_DETECT_INTERVAL_FRAMES);

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
            "Expected sdkconfig CPU frequency %d MHz, found %d MHz",
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
    s_initialized = true;
    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGW(
        TAG,
        "ADAPTIVE-POWER initialized: ACTIVE=%d MHz IDLE-SCAN=%d MHz "
        "idle_after=%u ms",
        APP_CPU_ACTIVE_FREQ_MHZ,
        APP_CPU_IDLE_SCAN_FREQ_MHZ,
        (unsigned)APP_CPU_IDLE_SCAN_AFTER_MS);

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
        ESP_LOGW(TAG, "IDLE-SCAN callbacks are not registered");
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
        ESP_LOGE(TAG, "IDLE-SCAN display suspend failed: %s",
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
        APP_CPU_IDLE_SCAN_FREQ_MHZ,
        CPU_POWER_STATE_IDLE_SCAN,
        inactive_ms);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IDLE-SCAN frequency failed; restoring ACTIVE display");
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
    s_state = CPU_POWER_STATE_IDLE_SCAN;
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

    if (previous_state == CPU_POWER_STATE_IDLE_SCAN) {
        /* Restore 360 MHz before asking the application to start MIPI-DSI. */
        ret = apply_fixed_frequency_locked(
            APP_CPU_ACTIVE_FREQ_MHZ,
            CPU_POWER_STATE_ACTIVE,
            0U);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cannot leave IDLE-SCAN because 360 MHz restore failed");
        xSemaphoreGive(s_policy_mutex);
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_state = CPU_POWER_STATE_ACTIVE;
    portEXIT_CRITICAL(&s_state_lock);

    /* This callback only schedules display restoration; it must not block. */
    if (s_exit_idle_scan != NULL) {
        ret = s_exit_idle_scan(s_transition_user_data);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ACTIVE display restore request failed: %s",
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

    if (inactive_ms >= APP_CPU_IDLE_SCAN_AFTER_MS) {
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
    return cpu_power_is_idle_scan_active()
        ? APP_FACE_DETECT_IDLE_INTERVAL_FRAMES
        : APP_FACE_DETECT_INTERVAL_FRAMES;
}

bool cpu_power_is_idle_scan_active(void)
{
    bool idle;
    portENTER_CRITICAL(&s_state_lock);
    idle = s_state != CPU_POWER_STATE_ACTIVE;
    portEXIT_CRITICAL(&s_state_lock);
    return idle;
}
