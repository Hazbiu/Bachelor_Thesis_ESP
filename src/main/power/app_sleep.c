#include "app_sleep.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "config/app_config.h"
#include "deep_sleep.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
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
    esp_sleep_wakeup_cause_t wake_cause)
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
    } else if (wake_cause == ESP_SLEEP_WAKEUP_GPIO) {
        /*
         * The GPIO3 press is an intentional Light-sleep wake. Consume that
         * press so the higher-priority Deep-sleep button task cannot interpret
         * the same electrical LOW level as a new Deep-sleep request.
         */
        s_ignore_button_until_release = true;

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

    /*
     * Power-profile mode: each stage is held for five seconds so that the
     * Joulescope graph can show a stable plateau for every subsystem.
     */
    ESP_LOGI("POWER_PROFILE", "STEP 0: all systems active");
    vTaskDelay(pdMS_TO_TICKS(5000));

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

    vTaskDelay(pdMS_TO_TICKS(5000));

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

    vTaskDelay(pdMS_TO_TICKS(5000));

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

    vTaskDelay(pdMS_TO_TICKS(5000));

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

    vTaskDelay(pdMS_TO_TICKS(5000));

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

    vTaskDelay(pdMS_TO_TICKS(5000));

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

    vTaskDelay(pdMS_TO_TICKS(5000));

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

        esp_err_t light_ret = enter_light_sleep(remaining_ms, true);
        esp_sleep_wakeup_cause_t wake_cause =
            light_sleep_get_last_wakeup_cause();

        finish_light_sleep(light_ret, wake_cause);

        if (light_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Light-sleep stage failed; staying active until Deep-sleep deadline: %s",
                esp_err_to_name(light_ret));
            continue;
        }

        if (wake_cause == ESP_SLEEP_WAKEUP_GPIO) {
            ESP_LOGI(
                TAG,
                "GPIO3 woke Light-sleep; inactivity window restarted");
        } else if (wake_cause == ESP_SLEEP_WAKEUP_TIMER) {
            ESP_LOGI(
                TAG,
                "Light-sleep timer reached the Deep-sleep inactivity deadline");
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
        3072,
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
