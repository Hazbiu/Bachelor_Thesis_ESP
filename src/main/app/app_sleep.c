#include "app/app_sleep.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "config/app_config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/camera/video_capture.h"
#include "power_save/component_audio.h"
#include "power_save/component_sdcard.h"
#include "power_save/component_wifi.h"
#include "power_save/deep_sleep.h"
#include "power_save/component_ethernet.h"

static const char *TAG = "app_sleep";

static portMUX_TYPE s_sleep_request_lock =
    portMUX_INITIALIZER_UNLOCKED;

static bool s_sleep_requested;
static bool s_inactivity_monitor_started;
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

    /*
     * The timeout check and the sleep claim share the same lock as the
     * face-detection reset. A face therefore cannot reset the timer between
     * the final timeout check and acceptance of the sleep request.
     */
    portENTER_CRITICAL(&s_sleep_request_lock);

    if (s_inactivity_monitor_started &&
        !s_sleep_requested &&
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
    }

    portEXIT_CRITICAL(&s_sleep_request_lock);
}

static void run_sleep_sequence(const char *reason)
{
    ESP_LOGI(
        TAG,
        "Deep sleep requested by %s",
        reason != NULL ? reason : "unknown source");

    /*
     * Power-profile mode:
     *
     * Every stage is held for five seconds so that the Joulescope graph
     * shows a stable current plateau after each subsystem is disabled.
     *
     * Total additional shutdown time: approximately 35 seconds.
     */
    ESP_LOGI(
        "POWER_PROFILE",
        "STEP 0: all systems active");

    vTaskDelay(pdMS_TO_TICKS(5000));

    /*
     * Stop new PPA, face-detection, face-recognition and LCD operations
     * before shutting down the camera stream.
     */
    if (s_prepare_callback != NULL) {
        s_prepare_callback(s_prepare_user_data);
    }

    /*
     * STEP 1:
     * Stop the V4L2 stream task, issue VIDIOC_STREAMOFF and close the
     * camera file descriptor.
     */
    ESP_LOGI(
        "POWER_PROFILE",
        "STEP 1: stopping camera");

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

    /*
     * STEP 2:
     * Switch the backlight off, stop LVGL and touch, switch LCD output
     * off, delete the LCD panel and MIPI-DSI bus, and release the
     * 2.5 V DPHY LDO.
     */
    ESP_LOGI(
        "POWER_PROFILE",
        "STEP 2: disabling display, touch and MIPI-DSI");

    esp_err_t display_ret =
        bsp_display_shutdown_for_deep_sleep();

    if (display_ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Display and MIPI-DSI shut down successfully");
    } else {
        ESP_LOGW(
            TAG,
            "Display shutdown completed with errors: %s",
            esp_err_to_name(display_ret));
    }

    vTaskDelay(pdMS_TO_TICKS(5000));

    /*
     * STEP 3:
     * Disable the board's external audio power amplifier.
     *
     * Waveshare ESP32-P4-NANO:
     * GPIO53 HIGH = amplifier enabled
     * GPIO53 LOW  = amplifier disabled
     */
    ESP_LOGI(
        "POWER_PROFILE",
        "STEP 3: disabling audio amplifier");

    esp_err_t audio_ret =
        component_audio_disable_for_deep_sleep();

    if (audio_ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Audio amplifier shut down successfully");
    } else {
        ESP_LOGW(
            TAG,
            "Audio amplifier shutdown failed: %s",
            esp_err_to_name(audio_ret));
    }

    vTaskDelay(pdMS_TO_TICKS(5000));

    /*
     * STEP 4:
     * Unmount the FAT filesystem and disconnect the microSD card's
     * SD1_VDD power rail.
     */
    ESP_LOGI(
        "POWER_PROFILE",
        "STEP 4: disabling microSD");

    esp_err_t sdcard_ret =
        component_sdcard_disable_for_deep_sleep();

    if (sdcard_ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "microSD unmounted and powered off successfully");
    } else {
        ESP_LOGW(
            TAG,
            "microSD shutdown failed: %s",
            esp_err_to_name(sdcard_ret));
    }

    vTaskDelay(pdMS_TO_TICKS(5000));

    /*
     * STEP 5:
     * Hold the external ESP32-C6 Wi-Fi coprocessor in reset.
     */
    ESP_LOGI(
        "POWER_PROFILE",
        "STEP 5: disabling ESP32-C6");

    esp_err_t wifi_ret =
        component_wifi_disable_for_deep_sleep();

    if (wifi_ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Wi-Fi coprocessor shut down successfully");
    } else {
        ESP_LOGW(
            TAG,
            "Wi-Fi coprocessor shutdown failed: %s",
            esp_err_to_name(wifi_ret));
    }

    /*
     * Keep the ESP32-C6 reset state active for five seconds so that
     * its current contribution can be measured separately.
     */
    vTaskDelay(pdMS_TO_TICKS(5000));

    /*
     * STEP 6:
     * Hold the onboard IP101GRI Ethernet PHY in hardware reset.
     *
     * Waveshare ESP32-P4-NANO:
     * GPIO51 HIGH = Ethernet PHY released
     * GPIO51 LOW  = Ethernet PHY held in reset
     */
    ESP_LOGI(
        "POWER_PROFILE",
        "STEP 6: disabling IP101GRI Ethernet PHY");

    esp_err_t ethernet_ret =
        component_ethernet_disable_for_deep_sleep();

    if (ethernet_ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Ethernet PHY held in reset successfully");
    } else {
        ESP_LOGW(
            TAG,
            "Ethernet PHY shutdown failed: %s",
            esp_err_to_name(ethernet_ret));
    }

    /*
     * Keep the Ethernet-reset state active for five seconds so that
     * its current contribution can be measured separately.
     */
    vTaskDelay(pdMS_TO_TICKS(5000));

    /*
     * STEP 7:
     * Configure the GPIO3 wake source and enter hardware deep sleep.
     */
    ESP_LOGI(
        "POWER_PROFILE",
        "STEP 7: entering ESP32-P4 deep sleep");

    enter_deep_sleep();

    /*
     * Normally unreachable. Release the Ethernet PHY only when
     * entering deep sleep unexpectedly fails.
     */
    esp_err_t ethernet_restore_ret =
        component_ethernet_restore_after_failed_sleep();

    if (ethernet_restore_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not restore Ethernet PHY: %s",
            esp_err_to_name(ethernet_restore_ret));
    }

    /*
     * Normally unreachable. Restore the audio amplifier only when
     * entering deep sleep unexpectedly fails.
     */
    esp_err_t audio_restore_ret =
        component_audio_restore_after_failed_sleep();

    if (audio_restore_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not restore audio amplifier after failed sleep: %s",
            esp_err_to_name(audio_restore_ret));
    }

    /*
     * Normally unreachable. Restore card power and remount it only when
     * entering deep sleep unexpectedly fails.
     */
    esp_err_t sdcard_restore_ret =
        component_sdcard_restore_after_failed_sleep();

    if (sdcard_restore_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not restore microSD after failed sleep: %s",
            esp_err_to_name(sdcard_restore_ret));
    }

    /*
     * Normally unreachable. Restore the ESP32-C6 only when entering
     * deep sleep unexpectedly fails.
     */
    esp_err_t wifi_restore_ret =
        component_wifi_restore_after_failed_sleep();

    if (wifi_restore_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not restore Wi-Fi coprocessor: %s",
            esp_err_to_name(wifi_restore_ret));
    }

    /*
     * Reached only when esp_deep_sleep_start() unexpectedly returns.
     */
    release_sleep_request();

    ESP_LOGE(
        TAG,
        "Deep sleep request returned without entering sleep");
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
        .pin_bit_mask =
            1ULL << APP_DEEP_SLEEP_BUTTON_GPIO,
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

    /*
     * A wake-up press may still be held while the application boots.
     * Wait for release so one press cannot wake the board and then
     * immediately put it back to sleep.
     */
    while (gpio_get_level(APP_DEEP_SLEEP_BUTTON_GPIO) == 0) {
        vTaskDelay(
            pdMS_TO_TICKS(APP_DEEP_SLEEP_BUTTON_POLL_MS));
    }

    vTaskDelay(
        pdMS_TO_TICKS(APP_DEEP_SLEEP_BUTTON_DEBOUNCE_MS));

    ESP_LOGI(
        TAG,
        "Deep-sleep button armed on GPIO%d",
        APP_DEEP_SLEEP_BUTTON_GPIO);

    while (true) {
        if (gpio_get_level(APP_DEEP_SLEEP_BUTTON_GPIO) == 0) {
            vTaskDelay(
                pdMS_TO_TICKS(
                    APP_DEEP_SLEEP_BUTTON_DEBOUNCE_MS));

            if (gpio_get_level(APP_DEEP_SLEEP_BUTTON_GPIO) == 0) {
                app_sleep_request("GPIO3 button");

                /*
                 * Reached only if entering deep sleep failed.
                 */
                while (
                    gpio_get_level(
                        APP_DEEP_SLEEP_BUTTON_GPIO) == 0) {
                    vTaskDelay(
                        pdMS_TO_TICKS(
                            APP_DEEP_SLEEP_BUTTON_POLL_MS));
                }
            }
        }

        vTaskDelay(
            pdMS_TO_TICKS(APP_DEEP_SLEEP_BUTTON_POLL_MS));
    }
}

static void deep_sleep_timeout_task(void *arg)
{
    (void)arg;

    ESP_LOGI(
        TAG,
        "Face-inactivity monitor armed: timeout=%d ms",
        APP_DEEP_SLEEP_TIMEOUT_MS);

    while (!app_sleep_is_requested()) {
        vTaskDelay(
            pdMS_TO_TICKS(
                APP_DEEP_SLEEP_INACTIVITY_POLL_MS));

        if (claim_inactivity_sleep_request()) {
            ESP_LOGI(
                TAG,
                "No face detected for %d ms; entering deep sleep",
                APP_DEEP_SLEEP_TIMEOUT_MS);

            run_sleep_sequence("30-second face inactivity");
            break;
        }
    }

    portENTER_CRITICAL(&s_sleep_request_lock);
    s_inactivity_monitor_started = false;
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
        ESP_LOGE(
            TAG,
            "Failed to create deep-sleep button task");

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

    portEXIT_CRITICAL(&s_sleep_request_lock);

    BaseType_t created = xTaskCreate(
        deep_sleep_timeout_task,
        "face_inactivity",
        2048,
        NULL,
        5,
        NULL);

    if (created != pdPASS) {
        portENTER_CRITICAL(&s_sleep_request_lock);
        s_inactivity_monitor_started = false;
        portEXIT_CRITICAL(&s_sleep_request_lock);

        ESP_LOGE(
            TAG,
            "Failed to create face-inactivity task");

        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
