#include "app/authentication/authentication_flow.h"

#include "app/controller/app_controller.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/esp32_p4_platform.h"
#include "config/app_config.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "domain/ports/system_adapters_port.h"
#include "domain/ports/presentation_port.h"
#include "app/authentication/authentication_session.h"
#include "services/power/power_manager.h"
#include "services/vision/face_recognizer.h"
#include "services/vision/face_result_store.h"


static const char *TAG = "app_main";
static authentication_flow_context_t s_context;


void authentication_flow_initialize(
    const authentication_flow_context_t *context)
{
    if (context != NULL) {
        s_context = *context;
    }
}


static SemaphoreHandle_t authentication_display_mode_mutex(void)
{
    return s_context.display_mode_mutex != NULL
        ? *s_context.display_mode_mutex
        : NULL;
}


static lv_display_t *authentication_display(void)
{
    return s_context.display != NULL
        ? *s_context.display
        : NULL;
}


static int authentication_video_fd(void)
{
    return s_context.video_fd != NULL
        ? *s_context.video_fd
        : -1;
}


static void authentication_set_display_buffer_index(
    uint8_t index)
{
    if (s_context.display_buffer_index != NULL) {
        *s_context.display_buffer_index = index;
    }
}


static void authentication_set_dummy_draw_enabled(
    bool enabled)
{
    if (s_context.dummy_draw_enabled != NULL) {
        *s_context.dummy_draw_enabled = enabled;
    }
}


static esp_err_t authentication_release_face_boost(
    const char *reason)
{
    if (s_context.release_face_boost == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return s_context.release_face_boost(reason);
}


static void authentication_mark_transition_failed(void)
{
    authentication_session_mark_transition_failed();

    authentication_release_face_boost("PIN transition failed");

    if (power_manager_inactivity_policy_is_paused()) {
        power_manager_resume_inactivity_policy();
    }

    /*
     * Camera operation remains the fallback when the PIN transition cannot
     * be completed. Keep the FSM synchronized with that existing behavior.
     */
    if (app_controller_get_state() == APP_STATE_AUTHENTICATING) {
        if (app_controller_handle_event(APP_EVENT_FACE_UNKNOWN)) {
            ESP_LOGI(
                TAG,
                "[APP-STATE] AUTHENTICATING -> CAMERA_ACTIVE "
                "(PIN transition failed)");
        }
    }
}

static void camera_resume_task(void *arg)
{
    (void)arg;

    /* Leave the success state visible briefly before restoring live video. */
    vTaskDelay(pdMS_TO_TICKS(350));

    if (power_manager_sleep_is_requested()) {
        vTaskDelete(NULL);
        return;
    }

    if (!authentication_display_mode_mutex() ||
        xSemaphoreTake(authentication_display_mode_mutex(), pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out while waiting to restore camera display mode");
        vTaskDelete(NULL);
        return;
    }

    /*
     * The camera stream is genuinely stopped for the PIN screen. Restore LVGL
     * ownership first, then restart capture while authentication still blocks
     * frame callbacks. Only after the stream is healthy do we reopen the
     * camera/display path to normal frames.
     */
    presentation_pin_hide();
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_err_t ret = esp_lv_adapter_set_dummy_draw(authentication_display(), true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Could not restore camera display mode: %s",
                 esp_err_to_name(ret));
        xSemaphoreGive(authentication_display_mode_mutex());
        vTaskDelete(NULL);
        return;
    }

    vision_face_result_store_clear();
    authentication_set_display_buffer_index(0);
    authentication_set_dummy_draw_enabled(true);

    ret = system_camera_stream_restart(authentication_video_fd());
    if (ret != ESP_OK) {
        authentication_set_dummy_draw_enabled(false);
        ESP_LOGE(TAG,
                 "Could not restart camera after PIN screen: %s",
                 esp_err_to_name(ret));
        xSemaphoreGive(authentication_display_mode_mutex());
        vTaskDelete(NULL);
        return;
    }

    authentication_session_mark_camera_active_after_pin();

    /*
     * The PIN was accepted and the camera stream has now been
     * successfully restored.
     *
     *     PIN_ENTRY -> CAMERA_ACTIVE
     *
     * Dispatch this only after system_camera_stream_restart()
     * succeeds so the FSM reflects the real application state.
     */
    if (app_controller_handle_event(APP_EVENT_PIN_COMPLETE)) {
        ESP_LOGI(
            TAG,
            "[APP-STATE] PIN_ENTRY -> CAMERA_ACTIVE");
    } else {
        ESP_LOGW(
            TAG,
            "[APP-STATE] PIN_COMPLETE did not cause a transition");
    }

    /*
     * Automatic Light/Deep timers restart only now, after live camera capture
     * and the AI-facing application state are genuinely active again.
     */
    power_manager_resume_inactivity_policy();
    xSemaphoreGive(authentication_display_mode_mutex());

    ESP_LOGI(TAG, "PIN accepted; camera stream and display mode restored");
    vTaskDelete(NULL);
}

static void pin_accepted_callback(void *user_data)
{
    (void)user_data;

    if (power_manager_sleep_is_requested()) {
        return;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        camera_resume_task,
        "camera_resume",
        4096,
        NULL,
        7,
        NULL,
        APP_SYSTEM_WORKER_CORE);

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create camera-resume task");
    }
}

static void pin_screen_transition_task(void *arg)
{
    (void)arg;

    if (!authentication_display_mode_mutex() ||
        xSemaphoreTake(authentication_display_mode_mutex(), pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out while waiting to show PIN screen");
        authentication_mark_transition_failed();
        vTaskDelete(NULL);
        return;
    }

    if (power_manager_sleep_is_requested()) {
        authentication_mark_transition_failed();
        xSemaphoreGive(authentication_display_mode_mutex());
        vTaskDelete(NULL);
        return;
    }

    /*
     * PIN entry is an explicit interaction state, not camera inactivity. Pause
     * automatic Light/Deep transitions before camera ownership changes. The
     * timer restarts with a fresh epoch only after camera restart succeeds.
     */
    if (!power_manager_pause_inactivity_policy()) {
        ESP_LOGW(
            TAG,
            "Could not pause automatic sleep policy before PIN transition");
        authentication_mark_transition_failed();
        xSemaphoreGive(authentication_display_mode_mutex());
        vTaskDelete(NULL);
        return;
    }

    /*
     * Stop the actual V4L2 stream for the PIN screen instead of merely ignoring
     * frame callbacks. This removes camera/ISP work while LVGL handles touch
     * input and prevents an old camera framebuffer/face box from being scanned
     * out underneath the PIN UI.
     */
    esp_err_t ret = system_camera_stream_stop(authentication_video_fd());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Could not pause camera for PIN screen: %s",
                 esp_err_to_name(ret));
        authentication_mark_transition_failed();
        xSemaphoreGive(authentication_display_mode_mutex());
        vTaskDelete(NULL);
        return;
    }

    vision_face_result_store_clear();
    authentication_set_display_buffer_index(0);

    ret = esp_lv_adapter_set_dummy_draw(authentication_display(), false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Could not enable LVGL PIN-screen mode: %s",
                 esp_err_to_name(ret));
        (void)system_camera_stream_restart(authentication_video_fd());
        authentication_mark_transition_failed();
        xSemaphoreGive(authentication_display_mode_mutex());
        vTaskDelete(NULL);
        return;
    }

    authentication_set_dummy_draw_enabled(false);

    char pending_identity[FACE_RECOG_MAX_NAME_LEN] = {0};

    authentication_session_copy_pending_identity(
        pending_identity,
        sizeof(pending_identity));

    ret = presentation_pin_show(
        pending_identity,
        pin_accepted_callback,
        NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not create PIN screen: %s", esp_err_to_name(ret));

        esp_err_t restore_ret = esp_lv_adapter_set_dummy_draw(authentication_display(), true);
        if (restore_ret == ESP_OK) {
            authentication_set_dummy_draw_enabled(true);
            (void)system_camera_stream_restart(authentication_video_fd());
        } else {
            ESP_LOGE(TAG,
                     "Could not recover camera display mode: %s",
                     esp_err_to_name(restore_ret));
        }

        authentication_mark_transition_failed();
        xSemaphoreGive(authentication_display_mode_mutex());
        vTaskDelete(NULL);
        return;
    }

    authentication_session_mark_pin_active();
    /* Camera rendering is paused in PIN mode. Restore the active brightness
     * here even if the last camera frame used a dimmed inactivity setting. */
    system_display_backlight_on();

    /*
     * presentation_pin_show() succeeded and completed its synchronous first
     * full-screen render, so the PIN UI now genuinely owns the application.
     *
     *     AUTHENTICATING -> PIN_ENTRY
     */
    if (app_controller_handle_event(APP_EVENT_FACE_RECOGNIZED)) {
        ESP_LOGI(
            TAG,
            "[APP-STATE] AUTHENTICATING -> PIN_ENTRY");
    } else {
        ESP_LOGW(
            TAG,
            "[APP-STATE] FACE_RECOGNIZED did not cause a transition");
    }

    /*
     * presentation_pin_show() performs a synchronous full-screen LVGL refresh before
     * returning. Keep the 360 MHz AI boost through that refresh, then return to
     * the normal 180 MHz baseline exactly as before.
     */
    const esp_err_t boost_release_ret =
        authentication_release_face_boost("PIN screen fully rendered");

    xSemaphoreGive(authentication_display_mode_mutex());
    ESP_LOGI(
        TAG,
        "Camera stream paused; PIN screen is active; CPU baseline release=%s",
        esp_err_to_name(boost_release_ret));
    vTaskDelete(NULL);
}

/*
 * Reserve the authentication transition and copy the identity only.
 *
 * The AI worker deliberately launches the PIN task AFTER releasing the AI
 * inference mutex. That creates a strict ordering:
 *
 *     detector -> recognizer -> unlock AI -> PIN transition
 *
 * and prevents the UI transition from racing the detector/recognizer chain
 * while the shared inference lock is still owned by the AI worker.
 */
bool authentication_flow_request_pin(const char *recognized_name)
{
    /*
     * Sleep remains an application/power concern.
     * Authentication state itself belongs to the session service.
     */
    if (power_manager_sleep_is_requested()) {
        return false;
    }

    return authentication_session_reserve_pin_identity(
        recognized_name);
}


bool authentication_flow_launch_pin_transition(void)
{
    BaseType_t created = xTaskCreatePinnedToCore(
        pin_screen_transition_task,
        "pin_screen",
        6144,
        NULL,
        7,
        NULL,
        APP_SYSTEM_WORKER_CORE);

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PIN-screen transition task");
        authentication_mark_transition_failed();
        return false;
    }

    return true;
}

void authentication_flow_on_recognition_started(void)
{
    /*
     * A real recognition pass is starting now.
     *
     *     CAMERA_ACTIVE -> AUTHENTICATING
     *
     * Do not move this transition to the CPU boost acquisition:
     * the boost begins before detection and therefore also covers
     * frames where no face is found.
     */
    if (app_controller_get_state() == APP_STATE_CAMERA_ACTIVE) {
        if (app_controller_handle_event(APP_EVENT_FACE_DETECTED)) {
            ESP_LOGI(
                TAG,
                "[APP-STATE] CAMERA_ACTIVE -> AUTHENTICATING");
        } else {
            ESP_LOGW(
                TAG,
                "[APP-STATE] FACE_DETECTED did not cause a transition");
        }
    }
}


void authentication_flow_on_recognition_not_authenticated(void)
{
    if (app_controller_get_state() == APP_STATE_AUTHENTICATING) {
        /*
         * This recognition pass did not proceed to second-factor
         * authentication. Return to normal live-camera state.
         */
        if (app_controller_handle_event(APP_EVENT_FACE_UNKNOWN)) {
            ESP_LOGI(
                TAG,
                "[APP-STATE] AUTHENTICATING -> CAMERA_ACTIVE");
        } else {
            ESP_LOGW(
                TAG,
                "[APP-STATE] FACE_UNKNOWN did not cause a transition");
        }
    }
}
