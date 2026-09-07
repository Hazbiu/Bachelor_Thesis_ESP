
#include "services/power/sleep/app_sleep.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/esp32_p4_platform.h"
#include "config/app_config.h"
#include "diagnostics/sleep_power_profile.h"
#include "services/power/sleep/deep_sleep.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "services/power/sleep/light_sleep.h"
#include "platform/camera/video_capture.h"
#include "platform/power/component_audio.h"
#include "platform/power/component_display.h"
#include "platform/power/component_ethernet.h"
#include "platform/power/component_sdcard.h"
#include "platform/power/component_wifi.h"
#include "platform/power/cpu_power.h"
#include "services/settings/app_settings.h"

#if APP_LIGHT_SLEEP_TOUCH_POLL_MS == 0
#error "APP_LIGHT_SLEEP_TOUCH_POLL_MS must be greater than zero"
#endif

/*
 * Build guard for the 250 ms responsiveness experiment. This deliberately
 * makes a stale/incorrect 1000 ms configuration fail at compile time instead
 * of silently producing the wrong measurement firmware.
 */
#if APP_LIGHT_SLEEP_TOUCH_POLL_MS != 250U
#error "GT911 Light-sleep experiment requires APP_LIGHT_SLEEP_TOUCH_POLL_MS == 250U"
#endif

#define APP_INACTIVITY_POWER_TASK_STACK_SIZE 8192

static const char *TAG = "app_sleep";
static const char *POWER_TAG = "PWR_STATE";

static portMUX_TYPE s_sleep_request_lock =
    portMUX_INITIALIZER_UNLOCKED;

static bool s_sleep_requested;
static bool s_inactivity_monitor_started;
static bool s_light_sleep_in_progress;
static bool s_ignore_button_until_release;
static bool s_light_sleep_failed_until_activity;
static int64_t s_last_face_detected_us;
static bool s_light_mode_enabled;
static bool s_deep_mode_enabled;
static uint32_t s_light_sleep_threshold_ms;
static uint32_t s_deep_sleep_threshold_ms;

static app_sleep_prepare_callback_t s_prepare_callback;
static void *s_prepare_user_data;
static app_sleep_light_transition_callback_t s_light_suspend_callback;
static app_sleep_light_transition_callback_t s_light_resume_callback;
static void *s_light_transition_user_data;

static void configure_runtime_power_modes(void)
{
    const app_settings_snapshot_t settings = app_settings_get();
    const bool light_enabled = settings.light_sleep_enabled;
    const bool deep_enabled = settings.deep_sleep_enabled;

    uint32_t light_threshold_ms = 0;
    uint32_t deep_threshold_ms = 0;

    if (light_enabled && deep_enabled) {
        light_threshold_ms = APP_LIGHT_SLEEP_TIMEOUT_MS;
        deep_threshold_ms =
            APP_LIGHT_SLEEP_TIMEOUT_MS +
            APP_POWER_MODES_LIGHT_TO_DEEP_GAP_MS;
    } else if (light_enabled) {
        light_threshold_ms = APP_SINGLE_SLEEP_TIMEOUT_MS;
    } else if (deep_enabled) {
        deep_threshold_ms = APP_SINGLE_SLEEP_TIMEOUT_MS;
    }

    portENTER_CRITICAL(&s_sleep_request_lock);
    s_light_mode_enabled = light_enabled;
    s_deep_mode_enabled = deep_enabled;
    s_light_sleep_threshold_ms = light_threshold_ms;
    s_deep_sleep_threshold_ms = deep_threshold_ms;
    portEXIT_CRITICAL(&s_sleep_request_lock);

    ESP_LOGI(
        POWER_TAG,
        "event=POWER_MODES_CONFIGURED light=%s deep=%s light_after_ms=%u "
        "deep_after_ms=%u light_to_deep_gap_ms=%u",
        light_enabled ? "ENABLED" : "DISABLED",
        deep_enabled ? "ENABLED" : "DISABLED",
        (unsigned)light_threshold_ms,
        (unsigned)deep_threshold_ms,
        (unsigned)(light_enabled && deep_enabled
            ? APP_POWER_MODES_LIGHT_TO_DEEP_GAP_MS
            : 0U));
}

/*
* External board peripherals are not controlled by the ESP32-P4's internal
* Light-sleep power-domain state machine. Quiesce only peripherals that can be
* restored in place without rebuilding application state. GT911 and the shared
* I2C bus remain active for touchscreen polling, and microSD remains powered and
* mounted so FATFS/VFS state and open application data survive Light-sleep.
*/
static void record_first_light_sleep_error(
    esp_err_t ret,
    esp_err_t *first_error)
{
    if (ret != ESP_OK && *first_error == ESP_OK) {
        *first_error = ret;
    }
}

static esp_err_t suspend_aux_peripherals_for_light_sleep(void)
{
    esp_err_t first_error = ESP_OK;

    /*
     * Do not hold or pulse the C6 reset in P4 Light-sleep. GPIO6->C6 GPIO2 is
     * raised while CHIP_PU stays HIGH; the already-running companion switches
     * to its 80 MHz policy in place. Deep-sleep keeps the existing self-sleep
     * boundary behavior.
     */
    record_first_light_sleep_error(
        component_wifi_prepare_for_light_sleep(),
        &first_error);

#if APP_LIGHT_SLEEP_DISABLE_AUDIO_AMP
    record_first_light_sleep_error(
        component_audio_disable_for_light_sleep(),
        &first_error);
#endif

#if APP_LIGHT_SLEEP_ETHERNET_WOL_STANDBY
    record_first_light_sleep_error(
        component_ethernet_enter_light_sleep_standby(),
        &first_error);
#endif

    if (first_error == ESP_OK) {
#if APP_LIGHT_SLEEP_DISABLE_AUDIO_AMP
        const char *audio_state = "OFF";
#else
        const char *audio_state = "UNCHANGED";
#endif
        const char *sdcard_state = "PRESERVED";
#if APP_LIGHT_SLEEP_ETHERNET_WOL_STANDBY
        const char *ethernet_state = "WOL_STANDBY_10M";
#else
        const char *ethernet_state = "UNCHANGED";
#endif

        ESP_LOGI(
            POWER_TAG,
            "event=LIGHT_SLEEP_AUX_SUSPENDED "
            "audio=%s sdcard=%s ethernet=%s "
            "c6=%s chip_pu=%s mode_gpio=%s gt911=POLLING",
            audio_state,
            sdcard_state,
            ethernet_state,
            component_wifi_is_enabled() ? "RETAINED_80MHZ_NO_RESET" : "OFF_BY_POLICY",
            component_wifi_is_enabled() ? "HIGH" : "LOW",
            component_wifi_is_enabled() ? "HIGH" : "LOW");
    }

    return first_error;
}

static esp_err_t restore_aux_peripherals_after_light_sleep(void)
{
    esp_err_t first_error = ESP_OK;

    record_first_light_sleep_error(
        component_wifi_restore_after_light_sleep(),
        &first_error);

    /*
    * Reverse only the reversible Light-sleep operations. Deep-sleep now uses
    * stronger one-way peripheral states (ES8311 register suspend and IP101GRI
    * BMCR Power Down) which are intentionally not used in this polling path.
    */
#if APP_LIGHT_SLEEP_ETHERNET_WOL_STANDBY
    record_first_light_sleep_error(
        component_ethernet_restore_after_light_sleep(),
        &first_error);
#endif

#if APP_LIGHT_SLEEP_DISABLE_AUDIO_AMP
    record_first_light_sleep_error(
        component_audio_restore_after_light_sleep(),
        &first_error);
#endif

    if (first_error == ESP_OK) {
#if APP_LIGHT_SLEEP_DISABLE_AUDIO_AMP
        const char *audio_state = "ON";
#else
        const char *audio_state = "UNCHANGED";
#endif
        const char *sdcard_state = "PRESERVED";
#if APP_LIGHT_SLEEP_ETHERNET_WOL_STANDBY
        const char *ethernet_state = "ACTIVE";
#else
        const char *ethernet_state = "UNCHANGED";
#endif

        ESP_LOGI(
            POWER_TAG,
            "event=LIGHT_SLEEP_AUX_RESTORED "
            "audio=%s sdcard=%s ethernet=%s c6=ACTIVE_POLICY mode_gpio=LOW",
            audio_state,
            sdcard_state,
            ethernet_state);
    }

    return first_error;
}

esp_err_t app_sleep_register_light_sleep_callbacks(
    app_sleep_light_transition_callback_t suspend_callback,
    app_sleep_light_transition_callback_t resume_callback,
    void *user_data)
{
    if (suspend_callback == NULL || resume_callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_sleep_request_lock);
    s_light_suspend_callback = suspend_callback;
    s_light_resume_callback = resume_callback;
    s_light_transition_user_data = user_data;
    portEXIT_CRITICAL(&s_sleep_request_lock);

    return ESP_OK;
}

static bool claim_sleep_request(void)
{
    bool claimed = false;

    portENTER_CRITICAL(&s_sleep_request_lock);

    if (!s_sleep_requested) {
        s_sleep_requested = true;
        claimed = true;
    }

    portEXIT_CRITICAL(&s_sleep_request_lock);
    return claimed;
}

static bool claim_inactivity_sleep_request(void)
{
    const int64_t now_us = esp_timer_get_time();
    bool claimed = false;

    portENTER_CRITICAL(&s_sleep_request_lock);

    const int64_t timeout_us =
        (int64_t)s_deep_sleep_threshold_ms * 1000LL;

    if (s_deep_mode_enabled &&
        s_deep_sleep_threshold_ms > 0 &&
        s_inactivity_monitor_started &&
        !s_sleep_requested &&
        !s_light_sleep_in_progress &&
        (now_us - s_last_face_detected_us) >= timeout_us) {
        s_sleep_requested = true;
        claimed = true;
    }

    portEXIT_CRITICAL(&s_sleep_request_lock);
    return claimed;
}

static void release_sleep_request(void)
{
    portENTER_CRITICAL(&s_sleep_request_lock);
    s_sleep_requested = false;
    portEXIT_CRITICAL(&s_sleep_request_lock);
}

bool app_sleep_is_requested(void)
{
    bool requested;

    portENTER_CRITICAL(&s_sleep_request_lock);
    requested = s_sleep_requested;
    portEXIT_CRITICAL(&s_sleep_request_lock);

    return requested;
}

void app_sleep_notify_face_detected(void)
{
    const int64_t now_us = esp_timer_get_time();
    bool activity_accepted = false;

    portENTER_CRITICAL(&s_sleep_request_lock);

    if (s_inactivity_monitor_started && !s_sleep_requested) {
        s_last_face_detected_us = now_us;
        s_light_sleep_failed_until_activity = false;
        activity_accepted = true;
    }

    portEXIT_CRITICAL(&s_sleep_request_lock);

    if (activity_accepted) {
        cpu_power_notify_activity();
    }
}

bool app_sleep_light_sleep_is_due(void)
{
    const int64_t now_us = esp_timer_get_time();
    bool due = false;

    portENTER_CRITICAL(&s_sleep_request_lock);
    const int64_t inactive_us = now_us - s_last_face_detected_us;
    const int64_t light_timeout_us =
        (int64_t)s_light_sleep_threshold_ms * 1000LL;
    const int64_t deep_timeout_us =
        (int64_t)s_deep_sleep_threshold_ms * 1000LL;

    due = s_light_mode_enabled &&
        s_light_sleep_threshold_ms > 0 &&
        s_inactivity_monitor_started &&
        !s_sleep_requested &&
        s_light_sleep_in_progress &&
        !s_light_sleep_failed_until_activity &&
        inactive_us >= light_timeout_us;

    if (s_deep_mode_enabled && s_deep_sleep_threshold_ms > 0) {
        due = due && inactive_us < deep_timeout_us;
    }

    portEXIT_CRITICAL(&s_sleep_request_lock);

    return due;
}

bool app_sleep_deep_mode_is_enabled(void)
{
    bool enabled;

    portENTER_CRITICAL(&s_sleep_request_lock);
    enabled = s_deep_mode_enabled;
    portEXIT_CRITICAL(&s_sleep_request_lock);

    return enabled;
}

static void cancel_light_sleep_claim(void)
{
    portENTER_CRITICAL(&s_sleep_request_lock);
    s_light_sleep_in_progress = false;
    portEXIT_CRITICAL(&s_sleep_request_lock);
}

static uint32_t get_inactivity_ms(void)
{
    const int64_t now_us = esp_timer_get_time();
    int64_t inactive_us = 0;

    portENTER_CRITICAL(&s_sleep_request_lock);

    if (s_inactivity_monitor_started && now_us > s_last_face_detected_us) {
        inactive_us = now_us - s_last_face_detected_us;
    }

    portEXIT_CRITICAL(&s_sleep_request_lock);

    return (uint32_t)(inactive_us / 1000LL);
}

static bool button_event_is_reserved_for_light_sleep(void)
{
    bool reserved;

    portENTER_CRITICAL(&s_sleep_request_lock);
    reserved = s_light_sleep_in_progress || s_ignore_button_until_release;
    portEXIT_CRITICAL(&s_sleep_request_lock);

    return reserved;
}

static void clear_button_release_guard(void)
{
    portENTER_CRITICAL(&s_sleep_request_lock);

    if (!s_light_sleep_in_progress) {
        s_ignore_button_until_release = false;
    }

    portEXIT_CRITICAL(&s_sleep_request_lock);
}

static void finish_light_sleep(
    esp_err_t light_ret,
    esp_sleep_wakeup_cause_t wake_cause,
    bool user_activity)
{
    const int64_t now_us = esp_timer_get_time();
    bool restore_performance = false;

    portENTER_CRITICAL(&s_sleep_request_lock);

    s_light_sleep_in_progress = false;

    if (light_ret != ESP_OK) {
        /*
        * Avoid retrying Light-sleep every 100 ms after a configuration or
        * hardware failure. Normal activity clears this guard.
        */
        s_light_sleep_failed_until_activity = true;
    } else if (user_activity) {
        if (wake_cause == ESP_SLEEP_WAKEUP_GPIO) {
            /*
            * Consume the GPIO3 wake press so the Deep-sleep button task does
            * not interpret the same electrical LOW as a second request.
            */
            s_ignore_button_until_release = true;
        }

        if (!s_sleep_requested) {
            s_last_face_detected_us = now_us;
            s_light_sleep_failed_until_activity = false;
            restore_performance = true;
        }
    }

    portEXIT_CRITICAL(&s_sleep_request_lock);

    if (restore_performance) {
        cpu_power_notify_activity();
    }
}

static void mark_light_sleep_recovery_failure(void)
{
    portENTER_CRITICAL(&s_sleep_request_lock);
    s_light_sleep_in_progress = false;
    s_light_sleep_failed_until_activity = true;
    portEXIT_CRITICAL(&s_sleep_request_lock);
}

static void recover_light_sleep_without_reset(
    const char *phase,
    esp_err_t cause,
    bool restore_aux,
    bool restore_hardware)
{
    mark_light_sleep_recovery_failure();

    ESP_LOGE(
        POWER_TAG,
        "event=LIGHT_SLEEP_FAILED phase=%s error=%s "
        "action=KEEP_ACTIVE_NO_RESET data_state=PRESERVED",
        phase != NULL ? phase : "UNKNOWN",
        esp_err_to_name(cause));

    if (restore_aux) {
        const esp_err_t aux_ret =
            restore_aux_peripherals_after_light_sleep();

        if (aux_ret != ESP_OK) {
            ESP_LOGE(
                POWER_TAG,
                "event=LIGHT_SLEEP_RECOVERY_WARNING phase=AUX_RESTORE "
                "error=%s action=CONTINUE_ACTIVE_NO_RESET",
                esp_err_to_name(aux_ret));
        }
    }

    if (restore_hardware && s_light_resume_callback != NULL) {
        const esp_err_t resume_ret =
            s_light_resume_callback(s_light_transition_user_data);

        if (resume_ret != ESP_OK) {
            ESP_LOGE(
                POWER_TAG,
                "event=LIGHT_SLEEP_RECOVERY_WARNING phase=HARDWARE_RESUME "
                "error=%s action=CONTINUE_ACTIVE_NO_RESET",
                esp_err_to_name(resume_ret));
        }
    }

    /*
     * Light-sleep must never become a reset boundary. Return the CPU policy to
     * ACTIVE and keep the existing RAM/application state. A later real activity
     * event clears the failure guard and may arm Light-sleep again.
     */
    cpu_power_notify_activity();
}

static bool claim_light_sleep_window(uint32_t *remaining_ms)
{
    const int64_t now_us = esp_timer_get_time();
    bool claimed = false;

    portENTER_CRITICAL(&s_sleep_request_lock);

    const int64_t inactive_us = now_us - s_last_face_detected_us;
    const int64_t light_timeout_us =
        (int64_t)s_light_sleep_threshold_ms * 1000LL;
    const int64_t deep_timeout_us =
        (int64_t)s_deep_sleep_threshold_ms * 1000LL;

    if (remaining_ms != NULL &&
        s_light_mode_enabled &&
        s_light_sleep_threshold_ms > 0 &&
        s_inactivity_monitor_started &&
        !s_sleep_requested &&
        !s_light_sleep_in_progress &&
        !s_light_sleep_failed_until_activity &&
        inactive_us >= light_timeout_us &&
        (!s_deep_mode_enabled || inactive_us < deep_timeout_us)) {
        if (s_deep_mode_enabled && s_deep_sleep_threshold_ms > 0) {
            const int64_t remaining_us = deep_timeout_us - inactive_us;
            *remaining_ms =
                (uint32_t)((remaining_us + 999LL) / 1000LL);
        } else {
            /* Light-only mode polls until real touch/GPIO3 activity. */
            *remaining_ms = 0;
        }
        s_light_sleep_in_progress = true;
        claimed = true;
    }

    portEXIT_CRITICAL(&s_sleep_request_lock);

    return claimed;
}

typedef enum {
    APP_DEEP_SLEEP_STRATEGY_AGGRESSIVE = 0,
    APP_DEEP_SLEEP_STRATEGY_PRESERVE_LIGHT_STATE,
} app_deep_sleep_strategy_t;


/*
 * Hybrid Light->Deep strategy.
 *
 * Preserve the exact external-peripheral state that already produced the
 * measured low Light-sleep current instead of "shutting down more" at the
 * Deep-sleep boundary.
 *
 * Preserved:
 *   camera       = reversible OV5647 Light-sleep standby
 *   display      = Light-sleep suspended
 *   audio amp    = OFF
 *   ES8311       = unchanged from Light-sleep
 *   microSD      = powered + mounted, no processing
 *   Ethernet     = RESET LOW
 *   GT911        = Light-sleep state
 *   shared I2C   = Light-sleep state
 *
 * Added:
 *   application/FSM Deep-sleep transition
 *   C6 Deep-sleep preparation
 *   GPIO3 wake source
 *   P4 esp_deep_sleep_start()
 */
static void run_light_compatible_deep_sleep_sequence(const char *reason)
{
    const char *sleep_reason =
        reason != NULL ? reason : "unknown source";

    ESP_LOGI(
        POWER_TAG,
        "event=RETENTIVE_DEEP_BEGIN reason=\"%s\" "
        "strategy=PRESERVE_LIGHT_SLEEP_NO_PERIODIC_WAKE",
        sleep_reason);

    ESP_LOGW(
        TAG,
        "RETENTIVE_DEEP selected for Hybrid mode: "
        "true ESP32-P4 Deep-sleep is intentionally NOT entered because the "
        "tested board draws more current after HP GPIO/peripheral power-down");

    /*
     * The camera, display and reversible auxiliary peripherals are already in
     * the measured low-current Light-sleep state. Do not call the destructive
     * Deep-sleep prepare callback here. Keep the application in LIGHT_SLEEP so
     * APP_EVENT_WAKE can restore camera/display in-place.
     */

#if APP_LIGHT_SLEEP_DISABLE_AUDIO_AMP
    esp_err_t audio_ret =
        component_audio_disable_for_light_sleep();

    if (audio_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not re-assert amplifier OFF before RETENTIVE_DEEP: %s",
            esp_err_to_name(audio_ret));
    }
#endif

#if APP_LIGHT_SLEEP_ETHERNET_WOL_STANDBY
    esp_err_t ethernet_ret =
        component_ethernet_enter_light_sleep_standby();

    if (ethernet_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not re-assert Ethernet RESET before RETENTIVE_DEEP: %s",
            esp_err_to_name(ethernet_ret));
    }
#endif

    /*
     * Keep C6 electrically OFF. This is lower-power than releasing CHIP_PU,
     * booting the C6, then asking it to enter its own Deep-sleep. The verified
     * C6 self-sleep firmware remains installed for the separate true/aggressive
     * P4 Deep-sleep path.
     */
    esp_err_t wifi_ret =
        component_wifi_disable_for_deep_sleep();

    if (wifi_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not re-assert C6 OFF before RETENTIVE_DEEP: %s",
            esp_err_to_name(wifi_ret));
    }

    /*
     * Reserve GPIO3 for this sleep cycle so the independent Deep-sleep button
     * task cannot consume the same active-low press after wake.
     */
    portENTER_CRITICAL(&s_sleep_request_lock);
    s_light_sleep_in_progress = true;
    portEXIT_CRITICAL(&s_sleep_request_lock);

    ESP_LOGI(
        POWER_TAG,
        "event=RETENTIVE_DEEP_ENTER "
        "p4_mode=LIGHT_SLEEP_CONTINUOUS "
        "timer=OFF touch_polling=OFF gpio3_wake=ON "
        "camera=LIGHT_STANDBY display=LIGHT_SUSPENDED "
        "audio_amp=OFF codec=PRESERVED sdcard=PRESERVED "
        "ethernet=RESET c6=HELD_OFF gt911=PRESERVED i2c=PRESERVED");

    ESP_LOGI(
        TAG,
        "Entering continuous Light-sleep with GPIO3 as the only wake source; "
        "no periodic timer slices and no GT911 polling");

    /*
     * timeout_ms=0 disables the timer wake source.
     * enable_gpio_wakeup=true leaves GPIO3 LOW as the only wake source.
     */
    const esp_err_t sleep_ret =
        enter_light_sleep(0, true);

    const esp_sleep_wakeup_cause_t wake_cause =
        light_sleep_get_last_wakeup_cause();

    /*
     * This path returns by design. Release the logical Deep request before
     * finish_light_sleep() so GPIO3 activity resets the inactivity timer and
     * restores the normal CPU policy.
     */
    release_sleep_request();

    const bool gpio_wake =
        sleep_ret == ESP_OK &&
        wake_cause == ESP_SLEEP_WAKEUP_GPIO;

    finish_light_sleep(
        sleep_ret,
        wake_cause,
        gpio_wake);

    if (sleep_ret != ESP_OK) {
        recover_light_sleep_without_reset(
            "RETENTIVE_DEEP_SLEEP_CALL",
            sleep_ret,
            true,
            true);

        ESP_LOGE(
            POWER_TAG,
            "event=RETENTIVE_DEEP_FAILED error=%s action=RESTORE_ACTIVE",
            esp_err_to_name(sleep_ret));
        return;
    }

    if (!gpio_wake) {
        recover_light_sleep_without_reset(
            "RETENTIVE_DEEP_UNEXPECTED_WAKE",
            ESP_ERR_INVALID_STATE,
            true,
            true);

        ESP_LOGE(
            POWER_TAG,
            "event=RETENTIVE_DEEP_FAILED wake_cause=%d "
            "action=RESTORE_ACTIVE",
            (int)wake_cause);
        return;
    }

    ESP_LOGI(
        POWER_TAG,
        "event=RETENTIVE_DEEP_EXIT reason=GPIO3 action=RESTORE_ACTIVE");

    esp_err_t aux_restore_ret =
        restore_aux_peripherals_after_light_sleep();

    if (aux_restore_ret != ESP_OK) {
        mark_light_sleep_recovery_failure();
        ESP_LOGE(
            POWER_TAG,
            "event=RETENTIVE_DEEP_RECOVERY_WARNING phase=AUX_RESTORE "
            "error=%s",
            esp_err_to_name(aux_restore_ret));
    }

    if (s_light_resume_callback != NULL) {
        esp_err_t resume_ret =
            s_light_resume_callback(s_light_transition_user_data);

        if (resume_ret != ESP_OK) {
            mark_light_sleep_recovery_failure();
            ESP_LOGE(
                POWER_TAG,
                "event=RETENTIVE_DEEP_RECOVERY_WARNING phase=HARDWARE_RESUME "
                "error=%s",
                esp_err_to_name(resume_ret));
        }
    }

    ESP_LOGI(
        TAG,
        "RETENTIVE_DEEP wake complete; application state restored without reboot");
}

static void run_aggressive_deep_sleep_sequence(const char *reason);


static void run_sleep_sequence(
    const char *reason,
    app_deep_sleep_strategy_t strategy)
{
    if (strategy == APP_DEEP_SLEEP_STRATEGY_PRESERVE_LIGHT_STATE &&
        !APP_SLEEP_POWER_PROFILE_TRUE_DEEP_FROM_LIGHT) {
        run_light_compatible_deep_sleep_sequence(reason);
        return;
    }

    sleep_power_profile_start(
        strategy == APP_DEEP_SLEEP_STRATEGY_PRESERVE_LIGHT_STATE
            ? "LIGHT_TO_TRUE_DEEP: external peripherals remain suspended"
            : "DIRECT_TRUE_DEEP");
    run_aggressive_deep_sleep_sequence(reason);
    sleep_power_profile_stop();
}


static void run_aggressive_deep_sleep_sequence(const char *reason)
{
    const char *sleep_reason =
        reason != NULL ? reason : "unknown source";

    ESP_LOGI(
        POWER_TAG,
        "event=DEEP_SLEEP_BEGIN reason=\"%s\"",
        sleep_reason);

    ESP_LOGI(
        TAG,
        "Deep sleep requested by %s",
        sleep_reason);

    sleep_power_profile_after(
        "Pre-shutdown baseline (P4 awake; peripheral state unchanged)",
        "REFERENCE_ONLY");

    /* Stop new PPA, AI and display work before camera shutdown. */
    sleep_power_profile_before("Application / AI drain / CPU boost release");
    if (s_prepare_callback != NULL) {
        s_prepare_callback(s_prepare_user_data);
    }
    sleep_power_profile_after(
        "Application / AI drain / CPU boost release",
        s_prepare_callback != NULL ? "CALLBACK_RETURNED_SEE_LOG" : "NO_CALLBACK");

    ESP_LOGI(TAG, "STEP 1: stopping camera");
    sleep_power_profile_before("OV5647 camera / CSI shutdown");
    esp_err_t camera_ret = app_video_shutdown();

    if (camera_ret == ESP_OK) {
        ESP_LOGI(TAG, "Camera shut down successfully");
    } else {
        ESP_LOGW(
            TAG,
            "Camera shutdown failed: %s",
            esp_err_to_name(camera_ret));
    }

    sleep_power_profile_after(
        "OV5647 camera / CSI shutdown",
        esp_err_to_name(camera_ret));

    /*
    * V20 STEP 2A: the physical JD9365 must receive DISPLAY_OFF + SLEEP_IN
    * while the BSP panel/DBI handle still exists. Deleting the MIPI-DSI
    * objects first would make the DCS Sleep-In command impossible.
    */
    ESP_LOGI(
        TAG,
        "STEP 2A: LCD controller DISPLAY_OFF + FULL SLEEP_IN");

    sleep_power_profile_before("JD9365 LCD full sleep / backlight OFF");
    esp_err_t lcd_sleep_ret = component_display_panel_enter_full_sleep();

    if (lcd_sleep_ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "LCD controller full Sleep-In requested successfully "
            "before MIPI-DSI teardown");
    } else {
        ESP_LOGW(
            TAG,
            "LCD controller full Sleep-In completed with errors: %s; "
            "continuing with display teardown",
            esp_err_to_name(lcd_sleep_ret));
    }

    sleep_power_profile_after(
        "JD9365 LCD full sleep / backlight OFF",
        esp_err_to_name(lcd_sleep_ret));

    ESP_LOGI(
        TAG,
        "STEP 2B: tearing down display, touch and MIPI-DSI");

    sleep_power_profile_before("Display transport / LVGL / MIPI-DSI teardown");
    esp_err_t display_ret = bsp_display_shutdown_for_deep_sleep();

    if (display_ret == ESP_OK) {
        ESP_LOGI(TAG, "Display and MIPI-DSI shut down successfully");
    } else {
        ESP_LOGW(
            TAG,
            "Display shutdown completed with errors: %s",
            esp_err_to_name(display_ret));
    }

    sleep_power_profile_after(
        "Display transport / LVGL / MIPI-DSI teardown",
        esp_err_to_name(display_ret));

    /*
    * STEP 3 runs after the BSP has released its own touch handle and while
    * the shared I2C bus is still alive. V20 retains V19's GT911
    * real full-Sleep command instead of Green mode. On the stock board there
    * is no P4-controlled GT911 INT/RESET wake pin, so this is a deepest-power
    * measurement policy: after GPIO3 wakes the P4, board power must be cycled
    * before touch is usable again.
    */
    ESP_LOGI(
        TAG,
        "STEP 3: GT911 FULL SLEEP + display side-channel cleanup "
        "(Green mode disabled; stock wiring needs power-cycle to restore touch)");

    sleep_power_profile_before("GT911 touch full sleep / display side channels");
    esp_err_t touch_ret = component_display_disable_for_deep_sleep();

    if (touch_ret == ESP_OK) {
        ESP_LOGI(TAG, "Touch controller and display pads prepared for sleep");
    } else {
        ESP_LOGW(
            TAG,
            "Display side-channel shutdown completed with errors: %s",
            esp_err_to_name(touch_ret));
    }

    sleep_power_profile_after(
        "GT911 touch full sleep / display side channels",
        esp_err_to_name(touch_ret));

    ESP_LOGI(
        TAG,
        "STEP 4: suspending ES8311 codec and disabling NS4150B amplifier");

    esp_err_t audio_ret = component_audio_disable_for_deep_sleep();

    if (audio_ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Audio subsystem Deep-sleep state verified: "
            "ES8311=SUSPEND NS4150B=OFF");
    } else {
        ESP_LOGW(
            TAG,
            "Audio Deep-sleep shutdown completed with errors: %s",
            esp_err_to_name(audio_ret));
    }

    ESP_LOGI(TAG, "STEP 5: disabling microSD");
    sleep_power_profile_before("microSD unmount / power OFF");
    esp_err_t sdcard_ret = component_sdcard_disable_for_deep_sleep();

    if (sdcard_ret == ESP_OK) {
        ESP_LOGI(TAG, "microSD unmounted and powered off successfully");
    } else {
        ESP_LOGW(
            TAG,
            "microSD shutdown failed: %s",
            esp_err_to_name(sdcard_ret));
    }

    sleep_power_profile_after(
        "microSD unmount / power OFF",
        esp_err_to_name(sdcard_ret));

    ESP_LOGI(TAG, "STEP 6: disabling ESP32-C6");
    sleep_power_profile_before("ESP32-C6 Wi-Fi / Bluetooth CHIP_PU LOW");
    esp_err_t wifi_ret = component_wifi_disable_for_deep_sleep();

    if (wifi_ret == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi coprocessor shut down successfully");
    } else {
        ESP_LOGW(
            TAG,
            "Wi-Fi coprocessor shutdown failed: %s",
            esp_err_to_name(wifi_ret));
    }

    sleep_power_profile_after(
        "ESP32-C6 Wi-Fi / Bluetooth CHIP_PU LOW",
        esp_err_to_name(wifi_ret));

    ESP_LOGI(
        TAG,
        "STEP 7: keeping IP101GRI in hardware RESET LOW for Deep-sleep");

    sleep_power_profile_before("IP101GRI Ethernet RESET LOW");
    esp_err_t ethernet_ret = component_ethernet_disable_for_deep_sleep();

    if (ethernet_ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Ethernet PHY Deep-sleep continuity verified: RESET LOW held");
    } else {
        ESP_LOGW(
            TAG,
            "Ethernet PHY hardware-reset shutdown failed: %s",
            esp_err_to_name(ethernet_ret));
    }

    sleep_power_profile_after(
        "IP101GRI Ethernet RESET LOW",
        esp_err_to_name(ethernet_ret));

    ESP_LOGI(TAG, "STEP 8: preparing final ESP32-P4 deep-sleep boundary");
    ESP_LOGI(
        POWER_TAG,
        "event=DEEP_SLEEP_BOUNDARY_PREPARE wake_gpio=%d wake_level=LOW",
        APP_DEEP_SLEEP_BUTTON_GPIO);
    enter_deep_sleep_with_profile(DEEP_SLEEP_PROFILE_AGGRESSIVE);

    sleep_power_profile_stop();

    /* The following recovery path is reached only if Deep-sleep fails. */
    esp_err_t ethernet_restore_ret =
        component_ethernet_restore_after_failed_sleep();
    if (ethernet_restore_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not restore Ethernet PHY: %s",
            esp_err_to_name(ethernet_restore_ret));
    }

    esp_err_t audio_restore_ret =
        component_audio_restore_after_failed_sleep();
    if (audio_restore_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not restore audio subsystem after failed sleep: %s",
            esp_err_to_name(audio_restore_ret));
    }

    esp_err_t sdcard_restore_ret =
        component_sdcard_restore_after_failed_sleep();
    if (sdcard_restore_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not restore microSD after failed sleep: %s",
            esp_err_to_name(sdcard_restore_ret));
    }

    esp_err_t wifi_restore_ret =
        component_wifi_restore_after_failed_sleep();
    if (wifi_restore_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not restore Wi-Fi coprocessor: %s",
            esp_err_to_name(wifi_restore_ret));
    }

    esp_err_t display_restore_ret =
        component_display_restore_after_failed_sleep();
    if (display_restore_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not release display side-channel holds: %s",
            esp_err_to_name(display_restore_ret));
    }

    release_sleep_request();
    ESP_LOGE(
        POWER_TAG,
        "event=DEEP_SLEEP_FAILED reason=ENTER_FUNCTION_RETURNED");
    ESP_LOGE(TAG, "Deep sleep request returned without entering sleep");
}

void app_sleep_request(const char *reason)
{
    const app_settings_snapshot_t settings = app_settings_get();
    if (!settings.deep_sleep_enabled) {
        ESP_LOGW(
            POWER_TAG,
            "event=DEEP_SLEEP_REQUEST_IGNORED policy=RUNTIME_DISABLED reason=\"%s\"",
            reason != NULL ? reason : "unknown");
        return;
    }

    if (!claim_sleep_request()) {
        return;
    }

    run_sleep_sequence(
        reason,
        APP_DEEP_SLEEP_STRATEGY_AGGRESSIVE);
}

static void deep_sleep_button_task(void *arg)
{
    (void)arg;

    gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << APP_DEEP_SLEEP_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        /*
        * GPIO3 is biased by the external pull-up fitted between GPIO3 and
        * ESP_3V3. Do not enable the internal pull-up in parallel.
        */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&button_config);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure sleep button: %s",
            esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    /* Do not reuse the wake-up press as a new request after boot. */
    while (gpio_get_level(APP_DEEP_SLEEP_BUTTON_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(APP_DEEP_SLEEP_BUTTON_POLL_MS));
    }

    vTaskDelay(pdMS_TO_TICKS(APP_DEEP_SLEEP_BUTTON_DEBOUNCE_MS));

    ESP_LOGI(
        TAG,
        "GPIO%d power-mode button armed; behavior follows saved Deep-sleep toggle",
        APP_DEEP_SLEEP_BUTTON_GPIO);

    while (true) {
        /*
        * GPIO3 is also the Light-sleep wake source. While Light-sleep is
        * active, or until its wake press is released, the button belongs to
        * the Light-sleep path and must not request Deep-sleep.
        */
        if (button_event_is_reserved_for_light_sleep()) {
            if (gpio_get_level(APP_DEEP_SLEEP_BUTTON_GPIO) != 0) {
                clear_button_release_guard();
            }

            vTaskDelay(pdMS_TO_TICKS(APP_DEEP_SLEEP_BUTTON_POLL_MS));
            continue;
        }

        if (gpio_get_level(APP_DEEP_SLEEP_BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(APP_DEEP_SLEEP_BUTTON_DEBOUNCE_MS));

            if (gpio_get_level(APP_DEEP_SLEEP_BUTTON_GPIO) == 0 &&
                !button_event_is_reserved_for_light_sleep()) {
                const app_settings_snapshot_t settings = app_settings_get();
                if (settings.deep_sleep_enabled) {
                    app_sleep_request("GPIO3 button");
                } else {
                    ESP_LOGI(
                        POWER_TAG,
                        "event=GPIO3_ACTIVITY deep_sleep=DISABLED action=KEEP_ACTIVE");
                    app_sleep_notify_face_detected();
                }

                while (gpio_get_level(APP_DEEP_SLEEP_BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(APP_DEEP_SLEEP_BUTTON_POLL_MS));
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(APP_DEEP_SLEEP_BUTTON_POLL_MS));
    }
}

static void inactivity_power_policy_task(void *arg)
{
    (void)arg;

    if (s_light_mode_enabled) {
        ESP_LOGI(
            POWER_TAG,
            "event=LIGHT_SLEEP_STATE_POLICY version=12 "
            "mode=RAM_PRESERVING_NO_RESET sdcard=PRESERVED gt911=POLLING "
            "touch_poll_ms=%u backlight=ON ethernet=WOL_STANDBY_10M",
            (unsigned)APP_LIGHT_SLEEP_TOUCH_POLL_MS);
    }

    const char *policy_name = s_light_mode_enabled
        ? (s_deep_mode_enabled ? "LIGHT_THEN_DEEP" : "LIGHT_ONLY")
        : (s_deep_mode_enabled ? "DEEP_ONLY" : "SLEEP_DISABLED");

    ESP_LOGI(
        POWER_TAG,
        "event=INACTIVITY_POLICY_STARTED policy=%s "
        "idle_180_after_ms=%u idle_90_after_ms=%u "
        "light_sleep_after_ms=%u deep_sleep_after_ms=%u",
        policy_name,
        (unsigned)APP_CPU_IDLE_180_AFTER_MS,
        (unsigned)APP_CPU_IDLE_90_AFTER_MS,
        (unsigned)s_light_sleep_threshold_ms,
        (unsigned)s_deep_sleep_threshold_ms);

    ESP_LOGI(
        TAG,
        "Runtime Power Modes armed: policy=%s light=%u ms deep=%u ms",
        policy_name,
        (unsigned)s_light_sleep_threshold_ms,
        (unsigned)s_deep_sleep_threshold_ms);

    while (!app_sleep_is_requested()) {
        vTaskDelay(pdMS_TO_TICKS(APP_DEEP_SLEEP_INACTIVITY_POLL_MS));

        /*
        * Apply the staged active CPU policy and reduce AI duty cycle before
        * entering the coordinated Light-sleep window.
        */
        const uint32_t inactive_ms = get_inactivity_ms();
        cpu_power_update_inactivity(inactive_ms);

        if (claim_inactivity_sleep_request()) {
            ESP_LOGI(
                POWER_TAG,
                "event=DEEP_SLEEP_DEADLINE_REACHED policy=%s "
                "inactive_ms=%" PRIu32 " threshold_ms=%u "
                "transition=ACTIVE_TO_DEEP action=SHUTDOWN",
                s_light_mode_enabled ? "LIGHT_THEN_DEEP" : "DEEP_ONLY",
                inactive_ms,
                (unsigned)s_deep_sleep_threshold_ms);
            ESP_LOGI(
                TAG,
                "No activity for %u ms; entering Deep-sleep directly "
                "without Light-sleep",
                (unsigned)s_deep_sleep_threshold_ms);

            run_sleep_sequence(
                "runtime Deep-sleep inactivity deadline",
                APP_DEEP_SLEEP_STRATEGY_AGGRESSIVE);
            break;
        }

        uint32_t remaining_ms = 0;
        if (!claim_light_sleep_window(&remaining_ms)) {
            continue;
        }

        ESP_LOGI(
            POWER_TAG,
            "event=LIGHT_SLEEP_BEGIN policy=%s inactive_ms=%" PRIu32
            " remaining_to_deep_ms=%" PRIu32,
            s_deep_mode_enabled ? "LIGHT_THEN_DEEP" : "LIGHT_ONLY",
            inactive_ms,
            remaining_ms);

        if (s_deep_mode_enabled) {
            ESP_LOGI(
                TAG,
                "No activity for %u ms; entering Light-sleep for up to %" PRIu32
                " ms before Deep-sleep",
                (unsigned)s_light_sleep_threshold_ms,
                remaining_ms);
        } else {
            ESP_LOGI(
                TAG,
                "No activity for %u ms; entering Light-sleep only until "
                "touch/GPIO3 activity",
                (unsigned)s_light_sleep_threshold_ms);
        }

        if (s_light_suspend_callback == NULL ||
            s_light_resume_callback == NULL) {
            cancel_light_sleep_claim();
            recover_light_sleep_without_reset(
                "CALLBACK_CHECK",
                ESP_ERR_INVALID_STATE,
                false,
                false);
            ESP_LOGE(
                TAG,
                "Light-sleep callbacks are not registered; "
                "remaining ACTIVE without resetting application state");
            continue;
        }

        if (!app_sleep_light_sleep_is_due()) {
            cancel_light_sleep_claim();
            ESP_LOGI(
                POWER_TAG,
                "event=LIGHT_SLEEP_CANCELED reason=ACTIVITY_BEFORE_SUSPEND");
            continue;
        }

        esp_err_t suspend_ret =
            s_light_suspend_callback(s_light_transition_user_data);

        if (suspend_ret == ESP_ERR_INVALID_STATE &&
            !app_sleep_light_sleep_is_due()) {
            cancel_light_sleep_claim();
            ESP_LOGI(
                POWER_TAG,
                "event=LIGHT_SLEEP_CANCELED reason=ACTIVITY_DURING_AI_DRAIN");
            continue;
        }

        if (suspend_ret != ESP_OK) {
            recover_light_sleep_without_reset(
                "HARDWARE_SUSPEND",
                suspend_ret,
                false,
                true);
            ESP_LOGE(
                TAG,
                "Application Light-sleep suspend failed: %s; "
                "remaining ACTIVE without reset",
                esp_err_to_name(suspend_ret));
            continue;
        }

        esp_err_t aux_suspend_ret =
            suspend_aux_peripherals_for_light_sleep();

        if (aux_suspend_ret != ESP_OK) {
            recover_light_sleep_without_reset(
                "AUX_SUSPEND",
                aux_suspend_ret,
                true,
                true);
            continue;
        }

        ESP_LOGI(
            POWER_TAG,
            "event=LIGHT_SLEEP_HARDWARE_SUSPENDED "
            "camera=OFF display=OFF backlight=ON audio=OFF sdcard=PRESERVED "
            "ethernet=WOL_STANDBY_10M c6=RETAINED_80MHZ "
            "gt911=POLLING ram=PRESERVED");

        /*
        * PRE-SLEEP CHECK
        *
        * The old code consumed just one GT911 sample here.  The V2 filter
        * proved that the controller is genuinely released before sleep, but
        * the hardware can still report a sustained false press immediately
        * after the first sleep slice.  Keep this pre-check as a sanity guard,
        * but DO NOT use it to arm touchscreen wake.
        *
        * Touch wake is armed only after a fresh post-sleep release streak.
        */
        esp_err_t touch_ret = ESP_OK;
        uint32_t pre_release_samples = 0;

        for (uint32_t attempt = 0;
            attempt < APP_LIGHT_SLEEP_TOUCH_PRECHECK_MAX_SAMPLES &&
            pre_release_samples < APP_LIGHT_SLEEP_TOUCH_PRE_RELEASE_SAMPLES;
            ++attempt) {
            bool touched_now = false;
            touch_ret = bsp_touch_poll_for_light_sleep(&touched_now);

            if (touch_ret != ESP_OK) {
                break;
            }

            if (touched_now) {
                pre_release_samples = 0;
            } else {
                pre_release_samples++;
            }

            if (pre_release_samples <
                APP_LIGHT_SLEEP_TOUCH_PRE_RELEASE_SAMPLES) {
                vTaskDelay(
                    pdMS_TO_TICKS(APP_LIGHT_SLEEP_TOUCH_SAMPLE_DELAY_MS));
            }
        }

        if (touch_ret != ESP_OK) {
            recover_light_sleep_without_reset(
                "TOUCH_PRECHECK",
                touch_ret,
                true,
                true);
            ESP_LOGE(
                TAG,
                "Could not prepare GT911 polling: %s; "
                "remaining ACTIVE without reset",
                esp_err_to_name(touch_ret));
            continue;
        }

        if (pre_release_samples <
            APP_LIGHT_SLEEP_TOUCH_PRE_RELEASE_SAMPLES) {
            /*
            * A real finger may have arrived while the hardware was being
            * suspended.  Restore ACTIVE instead of entering a sleep window
            * with an ambiguous precondition.
            */
            ESP_LOGI(
                POWER_TAG,
                "event=LIGHT_SLEEP_EXIT reason=TOUCHSCREEN_PRECHECK "
                "action=RESTORE_ACTIVE");

            finish_light_sleep(
                ESP_OK,
                ESP_SLEEP_WAKEUP_UNDEFINED,
                true);

            esp_err_t aux_restore_ret =
                restore_aux_peripherals_after_light_sleep();

            if (aux_restore_ret != ESP_OK) {
                mark_light_sleep_recovery_failure();
                ESP_LOGE(
                    POWER_TAG,
                    "event=LIGHT_SLEEP_RECOVERY_WARNING phase=AUX_RESTORE "
                    "error=%s action=CONTINUE_ACTIVE_NO_RESET",
                    esp_err_to_name(aux_restore_ret));
            }

            esp_err_t resume_ret =
                s_light_resume_callback(s_light_transition_user_data);

            if (resume_ret != ESP_OK) {
                mark_light_sleep_recovery_failure();
                ESP_LOGE(
                    POWER_TAG,
                    "event=LIGHT_SLEEP_RECOVERY_WARNING phase=HARDWARE_RESUME "
                    "error=%s action=CONTINUE_ACTIVE_NO_RESET",
                    esp_err_to_name(resume_ret));
            }

            continue;
        }

        ESP_LOGI(
            POWER_TAG,
            "event=LIGHT_SLEEP_TOUCH_PRECHECK "
            "released_samples=%" PRIu32,
            pre_release_samples);

        if (s_deep_mode_enabled) {
            ESP_LOGI(
                TAG,
                "GT911 polling every %u ms for %" PRIu32
                " ms before Deep-sleep; touch/GPIO3 returns to Active mode",
                (unsigned)APP_LIGHT_SLEEP_TOUCH_POLL_MS,
                remaining_ms);

            ESP_LOGI(
                POWER_TAG,
                "event=LIGHT_SLEEP_ENTER policy=LIGHT_THEN_DEEP "
                "mode=TIMER_SLICED poll_ms=%u backlight=ON remaining_to_deep_ms=%" PRIu32,
                (unsigned)APP_LIGHT_SLEEP_TOUCH_POLL_MS,
                remaining_ms);
        } else {
            ESP_LOGI(
                TAG,
                "GT911 polling every %u ms indefinitely; Deep-sleep is disabled",
                (unsigned)APP_LIGHT_SLEEP_TOUCH_POLL_MS);

            ESP_LOGI(
                POWER_TAG,
                "event=LIGHT_SLEEP_ENTER policy=LIGHT_ONLY "
                "mode=TIMER_SLICED_INDEFINITE poll_ms=%u backlight=ON deep_sleep=DISABLED",
                (unsigned)APP_LIGHT_SLEEP_TOUCH_POLL_MS);
        }

        esp_err_t light_ret = ESP_OK;
        esp_sleep_wakeup_cause_t wake_cause =
            ESP_SLEEP_WAKEUP_UNDEFINED;
        bool touchscreen_touched = false;
        bool touch_wake_armed = false;
        bool startup_press_logged = false;
        bool release_window_logged = false;
        bool press_candidate_logged = false;
        uint32_t light_sleep_elapsed_ms = 0;
        uint32_t release_stable_ms = 0;
        uint32_t press_stable_ms = 0;
        uint32_t short_slice_count = 0;

        while (!touchscreen_touched &&
               (!s_deep_mode_enabled || remaining_ms > 0)) {
            uint32_t poll_slice_ms = APP_LIGHT_SLEEP_TOUCH_POLL_MS;
            if (s_deep_mode_enabled && poll_slice_ms > remaining_ms) {
                poll_slice_ms = remaining_ms;
            }

            const int64_t slice_start_us = esp_timer_get_time();

            light_ret = enter_light_sleep_poll_slice(
                poll_slice_ms,
                true);
            wake_cause = light_sleep_get_last_wakeup_cause();

            const int64_t slice_elapsed_us =
                esp_timer_get_time() - slice_start_us;

            if (light_ret != ESP_OK ||
                wake_cause == ESP_SLEEP_WAKEUP_GPIO) {
                break;
            }

            if (wake_cause != ESP_SLEEP_WAKEUP_TIMER) {
                ESP_LOGE(
                    POWER_TAG,
                    "event=LIGHT_SLEEP_FAILED phase=WAKE_CHECK "
                    "wake_cause=%d action=RECOVER_ACTIVE_NO_RESET",
                    (int)wake_cause);
                light_ret = ESP_ERR_INVALID_STATE;
                break;
            }

            /*
            * The RTC can occasionally return materially before the requested
            * timer slice. Pad that shortfall while the application remains
            * suspended so the inactivity and touch qualification windows are
            * based on real scheduler time rather than a tight polling loop.
            */
            const int64_t requested_us = (int64_t)poll_slice_ms * 1000LL;
            const int64_t tolerance_us =
                (int64_t)APP_LIGHT_SLEEP_EARLY_RETURN_TOLERANCE_US;

            if (slice_elapsed_us + tolerance_us < requested_us) {
                const int64_t shortfall_us = requested_us - slice_elapsed_us;
                const uint32_t shortfall_ms =
                    (uint32_t)((shortfall_us + 999LL) / 1000LL);

                short_slice_count++;

                if (short_slice_count == 1) {
                    ESP_LOGW(
                        POWER_TAG,
                        "event=LIGHT_SLEEP_SLICE_MATERIALLY_SHORT "
                        "requested_ms=%" PRIu32 " actual_us=%" PRId64
                        " tolerance_us=%" PRId64 " action=PAD_WITH_DELAY",
                        poll_slice_ms,
                        slice_elapsed_us,
                        tolerance_us);
                    ESP_LOGW(
                        TAG,
                        "Light-sleep timer returned materially early: actual=%"
                        PRId64 " us requested=%" PRIu32
                        " ms. Padding the %" PRIu32
                        " ms shortfall so the inactivity deadline follows wall time.",
                        slice_elapsed_us,
                        poll_slice_ms,
                        shortfall_ms);
                }

                if (shortfall_ms > 0) {
                    vTaskDelay(pdMS_TO_TICKS(shortfall_ms));
                }
            }

            if (s_deep_mode_enabled) {
                remaining_ms -= poll_slice_ms;
            }

            if (UINT32_MAX - light_sleep_elapsed_ms < poll_slice_ms) {
                light_sleep_elapsed_ms = UINT32_MAX;
            } else {
                light_sleep_elapsed_ms += poll_slice_ms;
            }

            bool touched_now = false;
            touch_ret =
                bsp_touch_poll_for_light_sleep(&touched_now);

            if (touch_ret != ESP_OK) {
                ESP_LOGE(
                    POWER_TAG,
                    "event=LIGHT_SLEEP_FAILED phase=TOUCH_POLL "
                    "error=%s action=RECOVER_ACTIVE_NO_RESET",
                    esp_err_to_name(touch_ret));
                light_ret = touch_ret;
                break;
            }

            /*
            * V8 TIME-QUALIFIED TOUCH GATE (PRESERVED FROM V6)
            *
            * V4 proved that counting a few consecutive RELEASED samples was
            * insufficient: the GT911 could emit PRESSED -> RELEASED -> PRESSED
            * during the display/DSI power transition and satisfy the old gate.
            *
            * The V6/V8 managed-driver patch fixes the GT911 stale-point cache. This elapsed-time gate remains as a secondary filter:
            *
            *   startup quarantine
            *       ignore every GT911 PRESSED/RELEASED value
            *
            *   stable release window
            *       require continuously RELEASED for a configured duration
            *
            *   stable press window
            *       after arming, require continuously PRESSED for a configured
            *       duration before restoring the camera/display
            *
            * Any opposite sample resets the corresponding stability timer.
            * GPIO3 is independent and still wakes immediately.
            */
            if (light_sleep_elapsed_ms <
                APP_LIGHT_SLEEP_TOUCH_STARTUP_IGNORE_MS) {
                release_stable_ms = 0;
                press_stable_ms = 0;

                if (touched_now && !startup_press_logged) {
                    startup_press_logged = true;
                    ESP_LOGW(
                        POWER_TAG,
                        "event=LIGHT_SLEEP_TOUCH_STARTUP_PRESS_SUPPRESSED "
                        "elapsed_ms=%" PRIu32
                        " ignore_until_ms=%u action=CONTINUE_SLEEP",
                        light_sleep_elapsed_ms,
                        (unsigned)APP_LIGHT_SLEEP_TOUCH_STARTUP_IGNORE_MS);
                }

                continue;
            }

            if (!touch_wake_armed) {
                press_stable_ms = 0;
                press_candidate_logged = false;

                if (touched_now) {
                    release_stable_ms = 0;
                    release_window_logged = false;
                    continue;
                }

                if (!release_window_logged) {
                    release_window_logged = true;
                    ESP_LOGI(
                        POWER_TAG,
                        "event=LIGHT_SLEEP_TOUCH_RELEASE_WINDOW_STARTED "
                        "required_ms=%u",
                        (unsigned)APP_LIGHT_SLEEP_TOUCH_RELEASE_STABLE_MS);
                }

                if (UINT32_MAX - release_stable_ms < poll_slice_ms) {
                    release_stable_ms = UINT32_MAX;
                } else {
                    release_stable_ms += poll_slice_ms;
                }

                if (release_stable_ms >=
                    APP_LIGHT_SLEEP_TOUCH_RELEASE_STABLE_MS) {
                    touch_wake_armed = true;
                    press_stable_ms = 0;

                    ESP_LOGI(
                        POWER_TAG,
                        "event=LIGHT_SLEEP_TOUCH_POST_ARMED "
                        "stable_release_ms=%" PRIu32
                        " elapsed_ms=%" PRIu32,
                        release_stable_ms,
                        light_sleep_elapsed_ms);
                }

                continue;
            }

            if (!touched_now) {
                press_stable_ms = 0;
                press_candidate_logged = false;
                continue;
            }

            if (!press_candidate_logged) {
                press_candidate_logged = true;
                ESP_LOGI(
                    POWER_TAG,
                    "event=LIGHT_SLEEP_TOUCH_PRESS_CANDIDATE "
                    "required_ms=%u",
                    (unsigned)APP_LIGHT_SLEEP_TOUCH_PRESS_STABLE_MS);
            }

            if (UINT32_MAX - press_stable_ms < poll_slice_ms) {
                press_stable_ms = UINT32_MAX;
            } else {
                press_stable_ms += poll_slice_ms;
            }

            if (press_stable_ms >=
                APP_LIGHT_SLEEP_TOUCH_PRESS_STABLE_MS) {
                touchscreen_touched = true;
                ESP_LOGI(
                    POWER_TAG,
                    "event=LIGHT_SLEEP_TOUCH_CONFIRMED "
                    "stable_press_ms=%" PRIu32
                    " elapsed_ms=%" PRIu32
                    " action=RESTORE_ACTIVE",
                    press_stable_ms,
                    light_sleep_elapsed_ms);
            }
        }

        const bool user_activity = touchscreen_touched ||
            wake_cause == ESP_SLEEP_WAKEUP_GPIO;

        finish_light_sleep(light_ret, wake_cause, user_activity);

        if (light_ret != ESP_OK) {
            recover_light_sleep_without_reset(
                "SLEEP_CALL_OR_POLL",
                light_ret,
                true,
                true);
            ESP_LOGE(
                TAG,
                "Light-sleep failed after hardware suspend: %s; "
                "application state preserved and ACTIVE mode restored without reset",
                esp_err_to_name(light_ret));
            continue;
        }

        if (user_activity) {
            ESP_LOGI(
                POWER_TAG,
                "event=LIGHT_SLEEP_EXIT reason=%s action=RESTORE_ACTIVE",
                touchscreen_touched ? "TOUCHSCREEN" : "GPIO3");

            esp_err_t aux_restore_ret =
                restore_aux_peripherals_after_light_sleep();

            if (aux_restore_ret != ESP_OK) {
                mark_light_sleep_recovery_failure();
                ESP_LOGE(
                    POWER_TAG,
                    "event=LIGHT_SLEEP_RECOVERY_WARNING phase=AUX_RESTORE "
                    "error=%s action=CONTINUE_ACTIVE_NO_RESET",
                    esp_err_to_name(aux_restore_ret));
            }

            esp_err_t resume_ret =
                s_light_resume_callback(s_light_transition_user_data);

            if (resume_ret != ESP_OK) {
                mark_light_sleep_recovery_failure();
                ESP_LOGE(
                    POWER_TAG,
                    "event=LIGHT_SLEEP_RECOVERY_WARNING phase=HARDWARE_RESUME "
                    "error=%s action=CONTINUE_ACTIVE_NO_RESET",
                    esp_err_to_name(resume_ret));
                ESP_LOGE(
                    TAG,
                    "Application Light-sleep resume failed: %s; "
                    "keeping RAM/application state and continuing without reset",
                    esp_err_to_name(resume_ret));
            }

            if (touchscreen_touched) {
                ESP_LOGI(
                    TAG,
                    "Touchscreen activity restored camera/display without reboot");
            } else {
                ESP_LOGI(
                    TAG,
                    "GPIO3 woke Light-sleep; camera/display restored without reboot");
            }
        } else if (wake_cause == ESP_SLEEP_WAKEUP_TIMER &&
                s_deep_mode_enabled && remaining_ms == 0) {
            ESP_LOGI(
                POWER_TAG,
                "event=LIGHT_SLEEP_DEADLINE_REACHED policy=LIGHT_THEN_DEEP "
                "inactive_ms=%u action=DEEP_SLEEP",
                (unsigned)s_deep_sleep_threshold_ms);
            ESP_LOGI(
                TAG,
                "GT911 polling reached the %u ms Deep-sleep inactivity deadline",
                (unsigned)s_deep_sleep_threshold_ms);

            /*
            * Camera and display are already off. Claim the request directly
            * and continue into the destructive shutdown without powering
            * either pipeline back up for a single scheduler iteration.
            */
            if (claim_sleep_request()) {
                run_sleep_sequence(
                    "face inactivity after Light-sleep timer",
                    APP_DEEP_SLEEP_STRATEGY_PRESERVE_LIGHT_STATE);
            }

            /*
             * Normal RETENTIVE_DEEP returns after GPIO3 and restores RAM.
             * The diagnostic true-Deep path normally never returns. If entry
             * fails, its existing error/recovery path has logged the failure.
             */
            continue;
        }
    }

    portENTER_CRITICAL(&s_sleep_request_lock);
    s_inactivity_monitor_started = false;
    s_light_sleep_in_progress = false;
    portEXIT_CRITICAL(&s_sleep_request_lock);

    vTaskDelete(NULL);
}


esp_err_t app_sleep_start_button_monitor(
    app_sleep_prepare_callback_t prepare_callback,
    void *user_data)
{
    s_prepare_callback = prepare_callback;
    s_prepare_user_data = user_data;

    BaseType_t created = xTaskCreatePinnedToCore(
        deep_sleep_button_task,
        "deep_sleep_button",
        2048,
        NULL,
        APP_DEEP_SLEEP_BUTTON_PRIORITY,
        NULL,
        APP_SYSTEM_WORKER_CORE);

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create deep-sleep button task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t app_sleep_start_timeout(void)
{
    configure_runtime_power_modes();
    const int64_t start_time_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_sleep_request_lock);

    if (s_inactivity_monitor_started) {
        portEXIT_CRITICAL(&s_sleep_request_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_last_face_detected_us = start_time_us;
    s_inactivity_monitor_started = true;
    s_light_sleep_in_progress = false;
    s_ignore_button_until_release = false;
    s_light_sleep_failed_until_activity = false;

    portEXIT_CRITICAL(&s_sleep_request_lock);

    cpu_power_notify_activity();

    BaseType_t created = xTaskCreatePinnedToCore(
        inactivity_power_policy_task,
        "inactivity_power",
        APP_INACTIVITY_POWER_TASK_STACK_SIZE,
        NULL,
        5,
        NULL,
        APP_SYSTEM_WORKER_CORE);

    if (created != pdPASS) {
        portENTER_CRITICAL(&s_sleep_request_lock);
        s_inactivity_monitor_started = false;
        portEXIT_CRITICAL(&s_sleep_request_lock);

        ESP_LOGE(TAG, "Failed to create inactivity power-policy task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
