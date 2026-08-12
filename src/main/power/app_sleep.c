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
#include "power_save/component_ethernet.h"
#include "power_save/component_sdcard.h"
#include "power_save/component_wifi.h"

#if APP_LIGHT_SLEEP_TIMEOUT_MS >= APP_DEEP_SLEEP_TIMEOUT_MS
#error "APP_LIGHT_SLEEP_TIMEOUT_MS must be smaller than APP_DEEP_SLEEP_TIMEOUT_MS"
#endif

#if APP_LIGHT_SLEEP_TOUCH_POLL_MS == 0
#error "APP_LIGHT_SLEEP_TOUCH_POLL_MS must be greater than zero"
#endif

/*
 * Set this to 5000 only when deliberately recording per-subsystem current
 * plateaus. Normal firmware must proceed directly through the shutdown stages;
 * otherwise the old profiling delays keep the board awake for 35 seconds.
 */
#ifndef APP_SLEEP_POWER_PROFILE_STAGE_DELAY_MS
#define APP_SLEEP_POWER_PROFILE_STAGE_DELAY_MS 0
#endif

#define APP_INACTIVITY_POWER_TASK_STACK_SIZE 8192

static const char *TAG = "app_sleep";

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

    portENTER_CRITICAL(&s_sleep_request_lock);

    if (s_inactivity_monitor_started && !s_sleep_requested) {
        s_last_face_detected_us = now_us;
        s_light_sleep_failed_until_activity = false;
    }

    portEXIT_CRITICAL(&s_sleep_request_lock);
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
        }
    }

    portEXIT_CRITICAL(&s_sleep_request_lock);
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
    ESP_LOGI(
        TAG,
        "Deep sleep requested by %s",
        reason != NULL ? reason : "unknown source");

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

    ESP_LOGI("POWER_PROFILE", "STEP 3: disabling audio amplifier");
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

    ESP_LOGI("POWER_PROFILE", "STEP 4: disabling microSD");
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

    ESP_LOGI("POWER_PROFILE", "STEP 5: disabling ESP32-C6");
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
        "STEP 6: disabling IP101GRI Ethernet PHY");

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

    ESP_LOGI("POWER_PROFILE", "STEP 7: entering ESP32-P4 deep sleep");
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

    release_sleep_request();
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
        TAG,
        "Inactivity power policy armed: light=%d ms deep=%d ms",
        APP_LIGHT_SLEEP_TIMEOUT_MS,
        APP_DEEP_SLEEP_TIMEOUT_MS);

    while (!app_sleep_is_requested()) {
        vTaskDelay(pdMS_TO_TICKS(APP_DEEP_SLEEP_INACTIVITY_POLL_MS));

        if (claim_inactivity_sleep_request()) {
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
            TAG,
            "No activity for %d ms; entering Light-sleep for up to %" PRIu32 " ms",
            APP_LIGHT_SLEEP_TIMEOUT_MS,
            remaining_ms);

        if (s_light_suspend_callback == NULL ||
            s_light_resume_callback == NULL) {
            ESP_LOGE(
                TAG,
                "Light-sleep callbacks are not registered; restarting safely");
            esp_restart();
        }

        esp_err_t suspend_ret =
            s_light_suspend_callback(s_light_transition_user_data);

        if (suspend_ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Application Light-sleep suspend failed: %s; restarting safely",
                esp_err_to_name(suspend_ret));
            esp_restart();
        }

        /*
         * Flush any coordinate that LVGL consumed immediately before the
         * adapter was stopped. It must not be mistaken for a new wake touch.
         */
        bool stale_touch = false;
        esp_err_t touch_ret =
            bsp_touch_poll_for_light_sleep(&stale_touch);

        if (touch_ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Could not prepare GT911 polling: %s; restarting safely",
                esp_err_to_name(touch_ret));
            esp_restart();
        }

        ESP_LOGI(
            TAG,
            "GT911 polling armed every %d ms until the %d ms Deep-sleep deadline",
            APP_LIGHT_SLEEP_TOUCH_POLL_MS,
            APP_DEEP_SLEEP_TIMEOUT_MS);

        esp_err_t light_ret = ESP_OK;
        esp_sleep_wakeup_cause_t wake_cause =
            ESP_SLEEP_WAKEUP_UNDEFINED;
        bool touchscreen_touched = false;

        while (remaining_ms > 0 && !touchscreen_touched) {
            uint32_t poll_slice_ms = APP_LIGHT_SLEEP_TOUCH_POLL_MS;
            if (poll_slice_ms > remaining_ms) {
                poll_slice_ms = remaining_ms;
            }

            light_ret = enter_light_sleep_poll_slice(
                poll_slice_ms,
                true);
            wake_cause = light_sleep_get_last_wakeup_cause();

            if (light_ret != ESP_OK ||
                wake_cause == ESP_SLEEP_WAKEUP_GPIO) {
                break;
            }

            if (wake_cause != ESP_SLEEP_WAKEUP_TIMER) {
                ESP_LOGE(
                    TAG,
                    "Unexpected Light-sleep wake cause %d; restarting safely",
                    (int)wake_cause);
                esp_restart();
            }

            remaining_ms -= poll_slice_ms;

            touch_ret = bsp_touch_poll_for_light_sleep(
                &touchscreen_touched);

            if (touch_ret != ESP_OK) {
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
                TAG,
                "Light-sleep failed after hardware suspend: %s; restarting safely",
                esp_err_to_name(light_ret));
            esp_restart();
        }

        if (user_activity) {
            esp_err_t resume_ret =
                s_light_resume_callback(s_light_transition_user_data);

            if (resume_ret != ESP_OK) {
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
        1);

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

    BaseType_t created = xTaskCreate(
        inactivity_power_policy_task,
        "inactivity_power",
        APP_INACTIVITY_POWER_TASK_STACK_SIZE,
        NULL,
        5,
        NULL);

    if (created != pdPASS) {
        portENTER_CRITICAL(&s_sleep_request_lock);
        s_inactivity_monitor_started = false;
        portEXIT_CRITICAL(&s_sleep_request_lock);

        ESP_LOGE(TAG, "Failed to create inactivity power-policy task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
