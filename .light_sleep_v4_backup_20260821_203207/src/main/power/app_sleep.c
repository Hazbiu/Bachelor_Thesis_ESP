#include "app_sleep.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/esp32_p4_platform.h"
#include "config/app_config.h"
#include "deep_sleep.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "light_sleep.h"
#include "platform/camera/video_capture.h"
#include "power_save/component_audio.h"
#include "power_save/component_display.h"
#include "power_save/component_ethernet.h"
#include "power_save/component_sdcard.h"
#include "power_save/component_wifi.h"
#include "power_save/cpu_power.h"

#if APP_LIGHT_SLEEP_TIMEOUT_MS >= APP_DEEP_SLEEP_TIMEOUT_MS
#error "APP_LIGHT_SLEEP_TIMEOUT_MS must be smaller than APP_DEEP_SLEEP_TIMEOUT_MS"
#endif

#if APP_LIGHT_SLEEP_TOUCH_POLL_MS == 0
#error "APP_LIGHT_SLEEP_TOUCH_POLL_MS must be greater than zero"
#endif

#if APP_LIGHT_SLEEP_TOUCH_ARM_POLL_MS == 0
#error "APP_LIGHT_SLEEP_TOUCH_ARM_POLL_MS must be greater than zero"
#endif

#if APP_LIGHT_SLEEP_TOUCH_ARM_RELEASE_SAMPLES == 0
#error "APP_LIGHT_SLEEP_TOUCH_ARM_RELEASE_SAMPLES must be greater than zero"
#endif

#if APP_LIGHT_SLEEP_TOUCH_ARM_TIMEOUT_MS < APP_LIGHT_SLEEP_TOUCH_ARM_POLL_MS
#error "APP_LIGHT_SLEEP_TOUCH_ARM_TIMEOUT_MS must allow at least one arm poll"
#endif

/*
 * Defined in config/app_config.h. Set it to 5000 only when deliberately
 * recording per-subsystem current plateaus with a bench supply; the default of
 * 0 is mandatory for normal firmware, because a non-zero value keeps the board
 * fully awake for one extra delay per shutdown stage on every Deep-sleep entry.
 */
#ifndef APP_SLEEP_POWER_PROFILE_STAGE_DELAY_MS
#define APP_SLEEP_POWER_PROFILE_STAGE_DELAY_MS 0
#endif

#define APP_INACTIVITY_POWER_TASK_STACK_SIZE 8192

static const char *TAG = "app_sleep";
static const char *POWER_TAG = "PWR_STATE";

#define APP_LIGHT_SLEEP_TOUCH_FIX_VERSION 2

static portMUX_TYPE s_sleep_request_lock =
    portMUX_INITIALIZER_UNLOCKED;

static bool s_sleep_requested;
static bool s_inactivity_monitor_started;
static bool s_light_sleep_in_progress;
static bool s_ignore_button_until_release;
static bool s_light_sleep_failed_until_activity;
static int64_t s_last_face_detected_us;

static app_sleep_prepare_callback_t s_prepare_callback;
static void *s_prepare_user_data;
static app_sleep_light_transition_callback_t s_light_suspend_callback;
static app_sleep_light_transition_callback_t s_light_resume_callback;
static void *s_light_transition_user_data;

static void power_profile_stage_delay(void)
{
#if APP_SLEEP_POWER_PROFILE_STAGE_DELAY_MS > 0
    vTaskDelay(pdMS_TO_TICKS(APP_SLEEP_POWER_PROFILE_STAGE_DELAY_MS));
#endif
}


/*
 * Read the GT911 through the BSP's Light-sleep polling hook.  The BSP hook is
 * patched by this update to refresh the controller with
 * esp_lcd_touch_read_data() before inspecting coordinates; this wrapper keeps
 * all Light-sleep filtering in one place.
 */
static esp_err_t light_sleep_touch_poll(bool *touched)
{
    if (touched == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *touched = false;
    return bsp_touch_poll_for_light_sleep(touched);
}

/*
 * Establish a clean RELEASED baseline before the first Light-sleep slice.
 * A single stale coordinate must never count as new user activity.  If the
 * panel remains genuinely touched for the whole arm window, report it as
 * activity and cancel this Light-sleep attempt instead of sleeping under a
 * finger that is already down.
 */
static esp_err_t light_sleep_touch_arm(bool *touch_active)
{
    if (touch_active == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *touch_active = false;

    uint32_t stable_release_samples = 0;
    uint32_t elapsed_ms = 0;

    while (elapsed_ms <= APP_LIGHT_SLEEP_TOUCH_ARM_TIMEOUT_MS) {
        bool touched = false;
        esp_err_t ret = light_sleep_touch_poll(&touched);
        if (ret != ESP_OK) {
            return ret;
        }

        if (!touched) {
            stable_release_samples++;
            if (stable_release_samples >=
                APP_LIGHT_SLEEP_TOUCH_ARM_RELEASE_SAMPLES) {
                ESP_LOGI(
                    POWER_TAG,
                    "event=LIGHT_SLEEP_TOUCH_ARMED released_samples=%" PRIu32,
                    stable_release_samples);
                return ESP_OK;
            }
        } else {
            stable_release_samples = 0;
        }

        if (elapsed_ms >= APP_LIGHT_SLEEP_TOUCH_ARM_TIMEOUT_MS) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(APP_LIGHT_SLEEP_TOUCH_ARM_POLL_MS));
        elapsed_ms += APP_LIGHT_SLEEP_TOUCH_ARM_POLL_MS;
    }

    *touch_active = true;
    return ESP_OK;
}

/*
 * Accept a touchscreen wake only after two fresh positive samples.  The second
 * read is cheap and happens only when the first sample says "pressed".
 */
static esp_err_t light_sleep_touch_confirm(
    bool first_sample_touched,
    bool *confirmed)
{
    if (confirmed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *confirmed = false;

    if (!first_sample_touched) {
        return ESP_OK;
    }

    vTaskDelay(pdMS_TO_TICKS(APP_LIGHT_SLEEP_TOUCH_CONFIRM_DELAY_MS));

    bool second_sample_touched = false;
    esp_err_t ret = light_sleep_touch_poll(&second_sample_touched);
    if (ret != ESP_OK) {
        return ret;
    }

    *confirmed = second_sample_touched;

    if (!second_sample_touched) {
        ESP_LOGW(
            POWER_TAG,
            "event=LIGHT_SLEEP_TOUCH_GLITCH_REJECTED action=CONTINUE_SLEEP");
    }

    return ESP_OK;
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
    const int64_t timeout_us =
        (int64_t)APP_DEEP_SLEEP_TIMEOUT_MS * 1000LL;
    bool claimed = false;

    portENTER_CRITICAL(&s_sleep_request_lock);

    if (s_inactivity_monitor_started &&
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
    const int64_t light_timeout_us =
        (int64_t)APP_LIGHT_SLEEP_TIMEOUT_MS * 1000LL;
    const int64_t deep_timeout_us =
        (int64_t)APP_DEEP_SLEEP_TIMEOUT_MS * 1000LL;
    bool due = false;

    portENTER_CRITICAL(&s_sleep_request_lock);
    const int64_t inactive_us = now_us - s_last_face_detected_us;
    due = s_inactivity_monitor_started &&
          !s_sleep_requested &&
          s_light_sleep_in_progress &&
          !s_light_sleep_failed_until_activity &&
          inactive_us >= light_timeout_us &&
          inactive_us < deep_timeout_us;
    portEXIT_CRITICAL(&s_sleep_request_lock);

    return due;
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

static bool claim_light_sleep_window(uint32_t *remaining_ms)
{
    const int64_t now_us = esp_timer_get_time();
    const int64_t light_timeout_us =
        (int64_t)APP_LIGHT_SLEEP_TIMEOUT_MS * 1000LL;
    const int64_t deep_timeout_us =
        (int64_t)APP_DEEP_SLEEP_TIMEOUT_MS * 1000LL;
    bool claimed = false;

    portENTER_CRITICAL(&s_sleep_request_lock);

    const int64_t inactive_us = now_us - s_last_face_detected_us;

    if (remaining_ms != NULL &&
        s_inactivity_monitor_started &&
        !s_sleep_requested &&
        !s_light_sleep_in_progress &&
        !s_light_sleep_failed_until_activity &&
        inactive_us >= light_timeout_us &&
        inactive_us < deep_timeout_us) {
        const int64_t remaining_us = deep_timeout_us - inactive_us;

        *remaining_ms =
            (uint32_t)((remaining_us + 999LL) / 1000LL);
        s_light_sleep_in_progress = true;
        claimed = true;
    }

    portEXIT_CRITICAL(&s_sleep_request_lock);

    return claimed;
}

static void run_sleep_sequence(const char *reason)
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

    ESP_LOGI("POWER_PROFILE", "STEP 0: all systems active");
    power_profile_stage_delay();

    /* Stop new PPA, AI and display work before camera shutdown. */
    if (s_prepare_callback != NULL) {
        s_prepare_callback(s_prepare_user_data);
    }

    ESP_LOGI("POWER_PROFILE", "STEP 1: stopping camera");
    esp_err_t camera_ret = app_video_shutdown();

    if (camera_ret == ESP_OK) {
        ESP_LOGI(TAG, "Camera shut down successfully");
    } else {
        ESP_LOGW(
            TAG,
            "Camera shutdown failed: %s",
            esp_err_to_name(camera_ret));
    }

    power_profile_stage_delay();

    ESP_LOGI(
        "POWER_PROFILE",
        "STEP 2: disabling display, touch and MIPI-DSI");

    esp_err_t display_ret = bsp_display_shutdown_for_deep_sleep();

    if (display_ret == ESP_OK) {
        ESP_LOGI(TAG, "Display and MIPI-DSI shut down successfully");
    } else {
        ESP_LOGW(
            TAG,
            "Display shutdown completed with errors: %s",
            esp_err_to_name(display_ret));
    }

    power_profile_stage_delay();

    /*
     * STEP 3 runs after the BSP has released its own touch handle and while
     * the shared I2C bus is still alive. Without it the GT911 keeps scanning
     * the capacitive matrix at roughly 5..10 mA for the whole Deep-sleep
     * interval, which is typically the largest remaining "stuck" consumer on
     * this board once the camera and MIPI-DSI are down.
     */
    ESP_LOGI(
        "POWER_PROFILE",
        "STEP 3: sleeping GT911 touch controller and pinning display pads");

    esp_err_t touch_ret = component_display_disable_for_deep_sleep();

    if (touch_ret == ESP_OK) {
        ESP_LOGI(TAG, "Touch controller and display pads prepared for sleep");
    } else {
        ESP_LOGW(
            TAG,
            "Display side-channel shutdown completed with errors: %s",
            esp_err_to_name(touch_ret));
    }

    power_profile_stage_delay();

    ESP_LOGI("POWER_PROFILE", "STEP 4: disabling audio amplifier");
    esp_err_t audio_ret = component_audio_disable_for_deep_sleep();

    if (audio_ret == ESP_OK) {
        ESP_LOGI(TAG, "Audio amplifier shut down successfully");
    } else {
        ESP_LOGW(
            TAG,
            "Audio amplifier shutdown failed: %s",
            esp_err_to_name(audio_ret));
    }

    power_profile_stage_delay();

    ESP_LOGI("POWER_PROFILE", "STEP 5: disabling microSD");
    esp_err_t sdcard_ret = component_sdcard_disable_for_deep_sleep();

    if (sdcard_ret == ESP_OK) {
        ESP_LOGI(TAG, "microSD unmounted and powered off successfully");
    } else {
        ESP_LOGW(
            TAG,
            "microSD shutdown failed: %s",
            esp_err_to_name(sdcard_ret));
    }

    power_profile_stage_delay();

    ESP_LOGI("POWER_PROFILE", "STEP 6: disabling ESP32-C6");
    esp_err_t wifi_ret = component_wifi_disable_for_deep_sleep();

    if (wifi_ret == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi coprocessor shut down successfully");
    } else {
        ESP_LOGW(
            TAG,
            "Wi-Fi coprocessor shutdown failed: %s",
            esp_err_to_name(wifi_ret));
    }

    power_profile_stage_delay();

    ESP_LOGI(
        "POWER_PROFILE",
        "STEP 7: disabling IP101GRI Ethernet PHY");

    esp_err_t ethernet_ret = component_ethernet_disable_for_deep_sleep();

    if (ethernet_ret == ESP_OK) {
        ESP_LOGI(TAG, "Ethernet PHY held in reset successfully");
    } else {
        ESP_LOGW(
            TAG,
            "Ethernet PHY shutdown failed: %s",
            esp_err_to_name(ethernet_ret));
    }

    power_profile_stage_delay();

    ESP_LOGI("POWER_PROFILE", "STEP 8: entering ESP32-P4 deep sleep");
    ESP_LOGI(
        POWER_TAG,
        "event=DEEP_SLEEP_COMMIT wake_gpio=%d wake_level=LOW",
        APP_DEEP_SLEEP_BUTTON_GPIO);
    enter_deep_sleep();

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
            "Could not restore audio amplifier after failed sleep: %s",
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
    if (!claim_sleep_request()) {
        return;
    }

    run_sleep_sequence(reason);
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
            "Failed to configure deep-sleep button: %s",
            esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    /* Do not reuse the wake-up press as a new sleep request after boot. */
    while (gpio_get_level(APP_DEEP_SLEEP_BUTTON_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(APP_DEEP_SLEEP_BUTTON_POLL_MS));
    }

    vTaskDelay(pdMS_TO_TICKS(APP_DEEP_SLEEP_BUTTON_DEBOUNCE_MS));

    ESP_LOGI(
        TAG,
        "Deep-sleep button armed on GPIO%d",
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
                app_sleep_request("GPIO3 button");

                /* Reached only if entering Deep-sleep failed. */
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

    ESP_LOGI(
        POWER_TAG,
        "event=INACTIVITY_POLICY_STARTED idle_180_after_ms=%u "
        "idle_90_after_ms=%u light_sleep_after_ms=%u "
        "deep_sleep_after_ms=%u",
        (unsigned)APP_CPU_IDLE_180_AFTER_MS,
        (unsigned)APP_CPU_IDLE_90_AFTER_MS,
        (unsigned)APP_LIGHT_SLEEP_TIMEOUT_MS,
        (unsigned)APP_DEEP_SLEEP_TIMEOUT_MS);

    ESP_LOGI(
        TAG,
        "Inactivity power policy armed: light=%d ms deep=%d ms",
        APP_LIGHT_SLEEP_TIMEOUT_MS,
        APP_DEEP_SLEEP_TIMEOUT_MS);

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
                "event=DEEP_SLEEP_DEADLINE_REACHED inactive_ms=%" PRIu32
                " action=SHUTDOWN",
                inactive_ms);
            ESP_LOGI(
                TAG,
                "No activity for %d ms; entering Deep-sleep",
                APP_DEEP_SLEEP_TIMEOUT_MS);

            run_sleep_sequence("face inactivity after Light-sleep stage");
            break;
        }

        uint32_t remaining_ms = 0;
        if (!claim_light_sleep_window(&remaining_ms)) {
            continue;
        }

        ESP_LOGI(
            POWER_TAG,
            "event=LIGHT_SLEEP_BEGIN inactive_ms=%" PRIu32
            " remaining_to_deep_ms=%" PRIu32,
            inactive_ms,
            remaining_ms);

        ESP_LOGI(
            TAG,
            "No activity for %d ms; entering Light-sleep for up to %" PRIu32 " ms",
            APP_LIGHT_SLEEP_TIMEOUT_MS,
            remaining_ms);

        if (s_light_suspend_callback == NULL ||
            s_light_resume_callback == NULL) {
            ESP_LOGE(
                POWER_TAG,
                "event=LIGHT_SLEEP_FAILED phase=CALLBACK_CHECK "
                "error=NOT_REGISTERED action=RESTART");
            ESP_LOGE(
                TAG,
                "Light-sleep callbacks are not registered; restarting safely");
            esp_restart();
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
            ESP_LOGE(
                POWER_TAG,
                "event=LIGHT_SLEEP_FAILED phase=HARDWARE_SUSPEND "
                "error=%s action=RESTART",
                esp_err_to_name(suspend_ret));
            ESP_LOGE(
                TAG,
                "Application Light-sleep suspend failed: %s; restarting safely",
                esp_err_to_name(suspend_ret));
            esp_restart();
        }

        ESP_LOGI(
            POWER_TAG,
            "event=LIGHT_SLEEP_HARDWARE_SUSPENDED "
            "camera=OFF display=OFF");

        /*
         * Arm touch wake detection from a known RELEASED state.  The GT911 can
         * otherwise expose a cached coordinate that was last consumed by LVGL
         * immediately before the display adapter was stopped.
         */
        bool touch_active_while_arming = false;
        esp_err_t touch_ret =
            light_sleep_touch_arm(&touch_active_while_arming);

        if (touch_ret != ESP_OK) {
            ESP_LOGE(
                POWER_TAG,
                "event=LIGHT_SLEEP_FAILED phase=TOUCH_PREPARE "
                "error=%s action=RESTART",
                esp_err_to_name(touch_ret));
            ESP_LOGE(
                TAG,
                "Could not prepare GT911 polling: %s; restarting safely",
                esp_err_to_name(touch_ret));
            esp_restart();
        }

        if (touch_active_while_arming) {
            ESP_LOGI(
                POWER_TAG,
                "event=LIGHT_SLEEP_CANCELED reason=TOUCH_ACTIVE_DURING_ARM "
                "action=RESTORE_ACTIVE");

            finish_light_sleep(
                ESP_OK,
                ESP_SLEEP_WAKEUP_UNDEFINED,
                true);

            esp_err_t resume_ret =
                s_light_resume_callback(s_light_transition_user_data);
            if (resume_ret != ESP_OK) {
                ESP_LOGE(
                    POWER_TAG,
                    "event=LIGHT_SLEEP_FAILED phase=HARDWARE_RESUME "
                    "error=%s action=RESTART",
                    esp_err_to_name(resume_ret));
                ESP_LOGE(
                    TAG,
                    "Could not restore hardware after touch-arm cancel: %s; "
                    "restarting safely",
                    esp_err_to_name(resume_ret));
                esp_restart();
            }

            ESP_LOGI(
                TAG,
                "Touch remained active while arming; camera/display restored");
            continue;
        }

        ESP_LOGI(
            TAG,
            "GT911 polling armed every %d ms until the %d ms Deep-sleep deadline",
            APP_LIGHT_SLEEP_TOUCH_POLL_MS,
            APP_DEEP_SLEEP_TIMEOUT_MS);
        ESP_LOGI(
            POWER_TAG,
            "event=LIGHT_SLEEP_ENTER mode=TIMER_SLICED "
            "poll_ms=%u remaining_to_deep_ms=%" PRIu32,
            (unsigned)APP_LIGHT_SLEEP_TOUCH_POLL_MS,
            remaining_ms);

        esp_err_t light_ret = ESP_OK;
        esp_sleep_wakeup_cause_t wake_cause =
            ESP_SLEEP_WAKEUP_UNDEFINED;
        bool touchscreen_touched = false;
        uint32_t short_slice_count = 0;

        while (remaining_ms > 0 && !touchscreen_touched) {
            uint32_t poll_slice_ms = APP_LIGHT_SLEEP_TOUCH_POLL_MS;
            if (poll_slice_ms > remaining_ms) {
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
                    "wake_cause=%d action=RESTART",
                    (int)wake_cause);
                ESP_LOGE(
                    TAG,
                    "Unexpected Light-sleep wake cause %d; restarting safely",
                    (int)wake_cause);
                esp_restart();
            }

            /*
             * The RTC does not always sleep for the whole requested slice: a
             * call can return in a couple of milliseconds and still report a
             * TIMER wake. Charging the inactivity budget with the REQUESTED
             * slice in that case makes the countdown run far faster than real
             * time - observed on this board as the whole ~15 s Light-sleep
             * window being consumed in ~96 ms of wall clock, dropping the
             * system into Deep-sleep roughly 15 s early.
             *
             * Spend the remainder of the slice in a blocked delay so one loop
             * iteration always costs one slice of real time. The touch poll
             * cadence stays at APP_LIGHT_SLEEP_TOUCH_POLL_MS either way.
             */
            const int64_t requested_us = (int64_t)poll_slice_ms * 1000LL;
            const int64_t tolerance_us =
                (int64_t)APP_LIGHT_SLEEP_EARLY_RETURN_TOLERANCE_US;

            /*
             * Timer quantization routinely returns a 250 ms request a few
             * hundred microseconds early (for example ~249.4 ms). That is a
             * normal Light-sleep interval, not evidence that the RTC failed to
             * sleep. Only treat a return as materially short when it exceeds
             * the configured tolerance.
             */
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

            remaining_ms -= poll_slice_ms;

            bool touch_sample = false;
            touch_ret = light_sleep_touch_poll(&touch_sample);

            if (touch_ret == ESP_OK && touch_sample) {
                touch_ret = light_sleep_touch_confirm(
                    touch_sample,
                    &touchscreen_touched);
            } else {
                touchscreen_touched = false;
            }

            if (touch_ret != ESP_OK) {
                ESP_LOGE(
                    POWER_TAG,
                    "event=LIGHT_SLEEP_FAILED phase=TOUCH_POLL "
                    "error=%s action=RESTART",
                    esp_err_to_name(touch_ret));
                ESP_LOGE(
                    TAG,
                    "GT911 Light-sleep poll failed: %s; restarting safely",
                    esp_err_to_name(touch_ret));
                esp_restart();
            }
        }

        const bool user_activity = touchscreen_touched ||
            wake_cause == ESP_SLEEP_WAKEUP_GPIO;

        finish_light_sleep(light_ret, wake_cause, user_activity);

        if (light_ret != ESP_OK) {
            ESP_LOGE(
                POWER_TAG,
                "event=LIGHT_SLEEP_FAILED phase=SLEEP_CALL "
                "error=%s action=RESTART",
                esp_err_to_name(light_ret));
            ESP_LOGE(
                TAG,
                "Light-sleep failed after hardware suspend: %s; restarting safely",
                esp_err_to_name(light_ret));
            esp_restart();
        }

        if (user_activity) {
            ESP_LOGI(
                POWER_TAG,
                "event=LIGHT_SLEEP_EXIT reason=%s action=RESTORE_ACTIVE",
                touchscreen_touched ? "TOUCHSCREEN" : "GPIO3");
            esp_err_t resume_ret =
                s_light_resume_callback(s_light_transition_user_data);

            if (resume_ret != ESP_OK) {
                ESP_LOGE(
                    POWER_TAG,
                    "event=LIGHT_SLEEP_FAILED phase=HARDWARE_RESUME "
                    "error=%s action=RESTART",
                    esp_err_to_name(resume_ret));
                ESP_LOGE(
                    TAG,
                    "Application Light-sleep resume failed: %s; restarting safely",
                    esp_err_to_name(resume_ret));
                esp_restart();
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
                   remaining_ms == 0) {
            ESP_LOGI(
                POWER_TAG,
                "event=LIGHT_SLEEP_DEADLINE_REACHED inactive_ms=%u "
                "action=DEEP_SLEEP",
                (unsigned)APP_DEEP_SLEEP_TIMEOUT_MS);
            ESP_LOGI(
                TAG,
                "GT911 polling reached the %d ms Deep-sleep inactivity deadline",
                APP_DEEP_SLEEP_TIMEOUT_MS);

            /*
             * Camera and display are already off. Claim the request directly
             * and continue into the destructive shutdown without powering
             * either pipeline back up for a single scheduler iteration.
             */
            if (claim_sleep_request()) {
                run_sleep_sequence(
                    "face inactivity after Light-sleep timer");
            }
            break;
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

    ESP_LOGI(
        POWER_TAG,
        "event=LIGHT_SLEEP_TOUCH_FIX version=%d",
        APP_LIGHT_SLEEP_TOUCH_FIX_VERSION);

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
