
#include "app/navigation/app_navigation.h"

#include "app/camera/camera_session.h"
#include "app/controller/app_controller.h"
#include "config/app_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "domain/ports/presentation_port.h"


static const char *TAG = "app_main";

static void launcher_settings_requested(void *user_data);
static void settings_back_requested(void *user_data);

static void show_settings_task(void *arg)
{
    (void)arg;

    /* The launcher event has returned, so it is safe to take the LVGL mutex. */
    presentation_launcher_destroy();
    presentation_settings_create(settings_back_requested, NULL);

    /*
     * The Settings view now owns the application UI.
     *
     *     LAUNCHER -> SETTINGS
     */
    if (app_controller_handle_event(APP_EVENT_OPEN_SETTINGS)) {
        ESP_LOGI(TAG, "[APP-STATE] LAUNCHER -> SETTINGS");
    } else {
        ESP_LOGW(
            TAG,
            "[APP-STATE] OPEN_SETTINGS did not cause a transition");
    }

    vTaskDelete(NULL);
}

static void show_launcher_task(void *arg)
{
    (void)arg;

    /* Reload the launcher using the currently saved light/dark theme. */
    presentation_settings_destroy();
    presentation_launcher_create(
        camera_session_launcher_start_requested,
        launcher_settings_requested,
        NULL);

    /*
     * The launcher view now owns the application UI again.
     *
     *     SETTINGS -> LAUNCHER
     */
    if (app_controller_handle_event(APP_EVENT_SETTINGS_BACK)) {
        ESP_LOGI(TAG, "[APP-STATE] SETTINGS -> LAUNCHER");
    } else {
        ESP_LOGW(
            TAG,
            "[APP-STATE] SETTINGS_BACK did not cause a transition");
    }

    vTaskDelete(NULL);
}

static void launcher_settings_requested(void *user_data)
{
    (void)user_data;

    BaseType_t created = xTaskCreatePinnedToCore(
        show_settings_task,
        "show_settings",
        6144,
        NULL,
        5,
        NULL,
        APP_SYSTEM_WORKER_CORE);

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Settings navigation task");
    }
}

static void settings_back_requested(void *user_data)
{
    (void)user_data;

    BaseType_t created = xTaskCreatePinnedToCore(
        show_launcher_task,
        "show_launcher",
        4096,
        NULL,
        5,
        NULL,
        APP_SYSTEM_WORKER_CORE);

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create launcher navigation task");
    }
}


esp_err_t app_navigation_start(void)
{
    if (camera_session_prepare_launcher_display() != ESP_OK) {
        return ESP_FAIL;
    }

    presentation_launcher_create(
        camera_session_launcher_start_requested,
        launcher_settings_requested,
        NULL);

    if (!camera_session_launcher_touch_ready()) {
        presentation_launcher_show_error(
            "Touch controller unavailable. Check the touch cable and BSP display selection.");
    }

    camera_session_enable_launcher_backlight();

    /*
     * The launcher is now fully initialized and visible.
     *
     *     BOOTING -> LAUNCHER
     */
    if (app_controller_handle_event(APP_EVENT_BOOT_COMPLETE)) {
        ESP_LOGI(
            TAG,
            "[APP-STATE] BOOTING -> LAUNCHER");
    } else {
        ESP_LOGW(
            TAG,
            "[APP-STATE] BOOT_COMPLETE did not cause a transition");
    }

    return ESP_OK;
}
