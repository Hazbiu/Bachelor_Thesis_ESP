#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/ppa.h"
#include "services/vision/face_detector.h"
#include "services/vision/face_recognizer.h"
#include "services/vision/recognition_policy.h"
#include "services/vision/face_geometry.h"
#include "services/vision/face_detection_result_processor.h"
#include "services/vision/face_recognition_result_processor.h"
#include "services/vision/face_result_store.h"
#include "services/vision/ai_snapshot_buffer.h"
#include "services/vision/ai_snapshot_scheduler.h"
#include "services/vision/ai_worker_state.h"
#include "services/vision/ai_worker_runtime.h"
#include "services/vision/ai_worker_pipeline.h"
#include "services/vision/ai_inference_guard.h"
#include "diagnostics/core_trace.h"
#include "diagnostics/app_logging.h"
#include "diagnostics/cpu_stats.h"
#include "diagnostics/ai_pipeline_status.h"
#include "app/app_boot.h"
#include "app/camera/camera_session.h"
#include "app/configuration/app_configuration.h"
#include "domain/ports/presentation_port.h"
#include "domain/ports/system_adapters_port.h"
#include "app/controller/app_controller.h"
#include "app/authentication/authentication_flow.h"
#include "app/authentication/authentication_session.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/esp32_p4_platform.h"
#include "lvgl.h"
#include "lv_demos.h"
#include "services/power/sleep/wake_up.h"
#include "services/power/sleep/app_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "config/app_config.h"
#include "config/app_features.h"
#include "config/log_config.h"
#include "services/power/power_manager.h"

#define ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))

/*
 * Camera capture and LCD scanout are separate pipelines. Their buffer indices
 * must never be assumed to have the same phase, even when both pipelines use
 * the same number of buffers.
 *
 * Keep two camera buffers for V4L2 capture and two independent display buffers
 * for tear-free ping-pong rendering. The display buffer is handed to the LVGL
 * adapter with wait=true before that buffer is reused.
 */

/*
 * The PPA writes the display buffer through hardware, while the face rectangle
 * is written by the CPU. Invalidate before the CPU touches a PPA-written buffer,
 * then write it back before the LCD reads it.
 */

static void camera_video_frame_operation(
    uint8_t *camera_buf,
    uint8_t camera_buf_index,
    uint32_t camera_buf_hes,
    uint32_t camera_buf_ves,
    size_t camera_buf_len,
    void *user_data);

static void camera_video_frame_process(
    uint8_t *camera_buf,
    uint8_t camera_buf_index,
    uint32_t camera_buf_hes,
    uint32_t camera_buf_ves,
    size_t camera_buf_len,
    void *user_data);

static bool idle_scan_display_is_suspended(void);

static const char *TAG = "app_main";

static ppa_client_handle_t ppa_srm_handle = NULL;
static size_t data_cache_line_size = 0;
static void *display_buffer[APP_DISPLAY_BUFFER_COUNT];
static size_t lcd_fb_size = 0;
static uint8_t display_buffer_index = 0;
static uint8_t active_display_buffer_count = 0;
static bool display_backlight_enabled = false;
static lv_display_t *disp;
static lv_indev_t *launcher_touch_indev = NULL;
static int video_cam_fd0 = -1;
static bool display_suspended_for_light_sleep = false;
static volatile bool display_suspended_for_idle_scan = false;
static bool idle_scan_resume_task_pending = false;
static portMUX_TYPE idle_scan_state_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t frame_count = 0;
static int64_t s_last_preview_start_us = -1;
static int64_t s_last_ai_submit_us = -1;

/*
 * Live-preview FPS is measured from completed LCD blits, not from camera
 * capture callbacks or AI submissions.  This makes the on-screen value match
 * what the user actually sees.
 */
static int64_t s_preview_fps_window_start_us = -1;
static uint32_t s_preview_fps_window_frames = 0;
static uint32_t s_preview_fps_x10 = 0;

static bool work_interval_elapsed(int64_t now_us, int64_t last_us,
                                  uint32_t interval_ms)
{
    return last_us < 0 || now_us < last_us ||
        now_us - last_us >= (int64_t)interval_ms * 1000;
}

static void reset_preview_fps(void)
{
    s_preview_fps_window_start_us = -1;
    s_preview_fps_window_frames = 0;
    s_preview_fps_x10 = 0;
}

static void note_preview_frame_presented(int64_t now_us)
{
    if (s_preview_fps_window_start_us < 0 ||
        now_us < s_preview_fps_window_start_us) {
        s_preview_fps_window_start_us = now_us;
        s_preview_fps_window_frames = 1;
        return;
    }

    s_preview_fps_window_frames++;

    const int64_t elapsed_us =
        now_us - s_preview_fps_window_start_us;

    if (elapsed_us >= 1000000LL) {
        const uint64_t scaled_frames =
            (uint64_t)s_preview_fps_window_frames * 10000000ULL;

        s_preview_fps_x10 =
            (uint32_t)((scaled_frames + (uint64_t)elapsed_us / 2ULL) /
                       (uint64_t)elapsed_us);

        s_preview_fps_window_start_us = now_us;
        s_preview_fps_window_frames = 0;
    }
}

/*
 * AI runs on CPU1 from a private, downscaled snapshot. The camera/display
 * task on CPU0 never waits for BlazeFace or MobileFaceNet. There is exactly
 * one snapshot slot: while AI is busy, newer camera frames are displayed but
 * intentionally dropped from the AI path. This prevents an old-frame queue.
 */

static bool dummy_draw_enabled = false;
static bool application_start_requested = false;
static bool s_direct_deep_wake_start = false;

typedef enum {
    WAKE_TIMING_NONE = 0,
    WAKE_TIMING_LIGHT_SLEEP,
    WAKE_TIMING_DEEP_SLEEP,
} wake_timing_mode_t;

static wake_timing_mode_t s_wake_timing_mode = WAKE_TIMING_NONE;
static int64_t s_wake_timing_start_us = -1;

static bool dummy_mode_delay_flag = false;

/*
 * Direct camera scanout and normal LVGL rendering must never own the display
 * at the same time. The frame callback and both authentication transitions
 * use this mutex to serialize the ownership hand-off.
 */
static SemaphoreHandle_t display_mode_mutex;



static void startup_status(const char *message)
{
    if (s_direct_deep_wake_start) {
        ESP_LOGI(TAG, "[DEEP-WAKE] %s", message ? message : "");
        return;
    }

    presentation_launcher_set_status(message);
}

static void startup_error(const char *message)
{
    if (s_direct_deep_wake_start) {
        ESP_LOGE(TAG, "[DEEP-WAKE] startup error: %s", message ? message : "");
        return;
    }

    presentation_launcher_show_error(message);
}

#define WAKE_LOG_DIRECTORY "/sdcard/logs"

static const char *wake_timing_mode_name(wake_timing_mode_t mode)
{
    return mode == WAKE_TIMING_DEEP_SLEEP ? "DEEP_SLEEP" : "LIGHT_SLEEP";
}

static char wake_timing_mode_file_code(wake_timing_mode_t mode)
{
    return mode == WAKE_TIMING_DEEP_SLEEP ? 'D' : 'L';
}

static const char *wake_timing_scope(wake_timing_mode_t mode)
{
    return mode == WAKE_TIMING_DEEP_SLEEP
        ? "APP_RUNTIME_TO_FIRST_FRAME excludes_ROM_BOOTLOADER"
        : "WAKE_CONFIRMED_TO_FIRST_FRAME";
}

static const char *wake_timing_start_event(wake_timing_mode_t mode)
{
    return mode == WAKE_TIMING_DEEP_SLEEP
        ? "APP_RUNTIME_ENTRY"
        : "WAKE_CONFIRMED";
}

static uint32_t wake_timing_next_log_sequence(void)
{
    DIR *directory = opendir(WAKE_LOG_DIRECTORY);
    if (directory == NULL) {
        return 1U;
    }

    uint32_t highest_sequence = 0U;
    struct dirent *entry = NULL;

    while ((entry = readdir(directory)) != NULL) {
        char mode_code = '\0';
        unsigned sequence = 0U;
        int consumed = 0;

        /*
         * Use strict FAT 8.3 names so this works even when long filename
         * support is disabled in FatFs. Files are L000001.LOG/D000002.LOG.
         */
        if (sscanf(
                entry->d_name,
                "%c%6u.LOG%n",
                &mode_code,
                &sequence,
                &consumed) == 2 &&
            consumed == (int)strlen(entry->d_name) &&
            (mode_code == 'L' || mode_code == 'D' ||
             mode_code == 'l' || mode_code == 'd') &&
            sequence > highest_sequence) {
            highest_sequence = (uint32_t)sequence;
        }
    }

    closedir(directory);

    /* Six digits keep the basename within the FAT 8.3 limit. */
    return highest_sequence < 999999U
        ? highest_sequence + 1U
        : 0U;
}

static void wake_timing_write_log_file(
    wake_timing_mode_t mode,
    int64_t start_us,
    int64_t end_us,
    int64_t elapsed_us)
{
    /*
     * This runs only AFTER end_us has been captured, so SD/FAT filesystem work
     * is never included in the measured wake latency.
     */
    const esp_err_t mount_ret = system_storage_sdcard_ensure_mounted();
    if (mount_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "[WAKE-LOG] mode=%s not saved: microSD mount failed: %s",
            wake_timing_mode_name(mode),
            esp_err_to_name(mount_ret));
        return;
    }

    if (mkdir(WAKE_LOG_DIRECTORY, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(
            TAG,
            "[WAKE-LOG] mode=%s not saved: mkdir(%s) failed errno=%d",
            wake_timing_mode_name(mode),
            WAKE_LOG_DIRECTORY,
            errno);
        return;
    }

    const uint32_t sequence = wake_timing_next_log_sequence();
    if (sequence == 0U) {
        ESP_LOGW(TAG, "[WAKE-LOG] sequence counter exhausted");
        return;
    }

    char path[128];
    const int path_length = snprintf(
        path,
        sizeof(path),
        WAKE_LOG_DIRECTORY "/%c%06" PRIu32 ".LOG",
        wake_timing_mode_file_code(mode),
        sequence);

    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        ESP_LOGW(TAG, "[WAKE-LOG] path generation failed");
        return;
    }

    /*
     * The sequence is selected as one greater than the highest existing wake
     * file. This function is called synchronously by the single camera render
     * path, so there is no competing wake logger that can claim the same name.
     */
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        ESP_LOGW(
            TAG,
            "[WAKE-LOG] mode=%s not saved: fopen failed errno=%d path=%s",
            wake_timing_mode_name(mode),
            errno,
            path);
        return;
    }

    const double elapsed_ms = (double)elapsed_us / 1000.0;
    const double elapsed_sec = (double)elapsed_us / 1000000.0;

    fprintf(file, "wake_entry=%" PRIu32 "\n", sequence);
    fprintf(file, "mode=%s\n", wake_timing_mode_name(mode));
    fprintf(file, "start_event=%s\n", wake_timing_start_event(mode));
    fprintf(file, "end_event=FIRST_CAMERA_FRAME\n");
    fprintf(file, "start_us=%" PRId64 "\n", start_us);
    fprintf(file, "end_us=%" PRId64 "\n", end_us);
    fprintf(file, "elapsed_us=%" PRId64 "\n", elapsed_us);
    fprintf(file, "elapsed_ms=%.3f\n", elapsed_ms);
    fprintf(file, "elapsed_sec=%.6f\n", elapsed_sec);
    fprintf(file, "scope=%s\n", wake_timing_scope(mode));

    const int close_ret = fclose(file);
    if (close_ret != 0) {
        ESP_LOGW(
            TAG,
            "[WAKE-LOG] mode=%s file close/flush failed errno=%d path=%s",
            wake_timing_mode_name(mode),
            errno,
            path);
        return;
    }

    ESP_LOGI(
        TAG,
        "[WAKE-LOG] saved entry=%u mode=%s path=%s elapsed_sec=%.6f",
        sequence,
        wake_timing_mode_name(mode),
        path,
        elapsed_sec);
}

static void wake_timing_arm(
    wake_timing_mode_t mode,
    int64_t start_us)
{
    if (start_us < 0) {
        return;
    }

    s_wake_timing_mode = mode;
    s_wake_timing_start_us = start_us;

    ESP_LOGI(
        TAG,
        "[WAKE-TIME] mode=%s event=TIMER_ARMED timestamp_us=%" PRId64,
        wake_timing_mode_name(mode),
        start_us);
}

static void wake_timing_finish_on_first_camera_frame(void)
{
    if (s_wake_timing_mode == WAKE_TIMING_NONE ||
        s_wake_timing_start_us < 0) {
        return;
    }

    /*
     * Capture and disarm FIRST. File I/O happens afterwards and therefore can
     * neither change the measured endpoint nor create a duplicate log on a
     * later camera frame.
     */
    const wake_timing_mode_t completed_mode = s_wake_timing_mode;
    const int64_t start_us = s_wake_timing_start_us;
    const int64_t end_us = esp_timer_get_time();
    const int64_t elapsed_us = end_us - start_us;

    s_wake_timing_mode = WAKE_TIMING_NONE;
    s_wake_timing_start_us = -1;

    ESP_LOGI(
        TAG,
        "[WAKE-TIME] mode=%s event=FIRST_CAMERA_FRAME "
        "start_us=%" PRId64 " end_us=%" PRId64
        " elapsed_us=%" PRId64 " elapsed_ms=%.3f elapsed_sec=%.6f"
        " scope=%s",
        wake_timing_mode_name(completed_mode),
        start_us,
        end_us,
        elapsed_us,
        (double)elapsed_us / 1000.0,
        (double)elapsed_us / 1000000.0,
        wake_timing_scope(completed_mode));

    wake_timing_write_log_file(
        completed_mode,
        start_us,
        end_us,
        elapsed_us);
}

esp_err_t camera_session_prepare_launcher_display(void)
{
    /*
     * Phase 1: normal LVGL rendering. The user sees a proper launcher and the
     * camera/AI pipeline is not initialized until Start Camera is pressed.
     */
    disp = system_display_start();
    if (!disp) {
        ESP_LOGE(TAG,
                 "Display/touch initialization failed. Check the selected Waveshare "
                 "display in menuconfig and the touch cable.");
        return ESP_FAIL;
    }

    /*
     * bsp_display_start() starts the LVGL worker asynchronously. Normal LVGL
     * mode is already active at boot, so do not force another dummy-mode
     * transition here.
     */
    vTaskDelay(pdMS_TO_TICKS(150));

    /* The display startup creates the shared BSP I2C bus used by ES8311. */
    esp_err_t post_display_policy_ret = app_configuration_apply_saved_policy();
    if (post_display_policy_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Post-display power policy application failed: %s",
            esp_err_to_name(post_display_policy_ret));
    }

    system_display_disable_lvgl_overlays(disp);

    /*
     * The BSP initializes GT911 and registers the LVGL input device inside
     * bsp_display_start(). Use the BSP-owned handle directly.
     */
    launcher_touch_indev = system_display_get_input_device();
    if (launcher_touch_indev) {
        ESP_LOGI(TAG,
                 "BSP touchscreen ready: type=%d",
                 (int)lv_indev_get_type(launcher_touch_indev));
    } else {
        ESP_LOGE(TAG, "BSP did not return an LVGL touchscreen input device");
    }

    return ESP_OK;
}



esp_err_t camera_session_prepare_direct_wake_display(void)
{
    /*
     * Mark the boot as the launcher-free Deep-sleep path before any shared
     * display/power setup can report an error. This keeps every failure path
     * serial-only because no launcher widgets exist on this boot.
     */
    s_direct_deep_wake_start = true;

    const esp_err_t ret = camera_session_prepare_launcher_display();
    if (ret != ESP_OK) {
        return ret;
    }

    /*
     * The BSP display stack must exist before the camera path can be built, but
     * no launcher widgets are created on a Deep-sleep GPIO3 wake. Keep the
     * physical display dark until the first complete live camera frame.
     */
    system_display_backlight_off();
    display_backlight_enabled = false;

    ESP_LOGI(
        TAG,
        "[DEEP-WAKE] display/touch stack prepared with launcher UI BYPASSED; "
        "backlight remains OFF until first camera frame");

    return ESP_OK;
}

bool camera_session_launcher_touch_ready(void)
{
    return launcher_touch_indev != NULL;
}


void camera_session_enable_launcher_backlight(void)
{
    system_display_backlight_on();
    display_backlight_enabled = true;
}

static void log_psram_state(const char *stage)
{
    const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    ESP_LOGI(
        TAG,
        "PSRAM stage=%s free=%u largest=%u display_active=%u",
        stage ? stage : "unknown",
        (unsigned)free_bytes,
        (unsigned)largest_block,
        (unsigned)active_display_buffer_count);
}

static esp_err_t allocate_display_buffers(void)
{
    if (data_cache_line_size == 0 || lcd_fb_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t available = 0;

    for (int i = 0; i < APP_DISPLAY_BUFFER_COUNT; i++) {
        if (display_buffer[i] == NULL) {
            display_buffer[i] = heap_caps_aligned_calloc(
                data_cache_line_size,
                1,
                lcd_fb_size,
                MALLOC_CAP_SPIRAM);
        }

        if (display_buffer[i] == NULL) {
            if (i == 0) {
                active_display_buffer_count = 0;
                ESP_LOGE(TAG, "Failed to allocate required display buffer 0");
                log_psram_state("display_alloc_failed_required");
                return ESP_ERR_NO_MEM;
            }

            /*
             * A second application ping-pong buffer is desirable but not
             * required for correctness because dummy_draw_blit() is called
             * with wait=true. After MIPI-DSI recreation PSRAM can be highly
             * fragmented, so keep the live preview running with one source
             * buffer instead of rebooting.
             */
            ESP_LOGW(
                TAG,
                "Display buffer %d unavailable after resume; continuing with %u buffer(s)",
                i,
                (unsigned)available);
            break;
        }

        available++;
    }

    if (available == 0) {
        active_display_buffer_count = 0;
        return ESP_ERR_NO_MEM;
    }

    active_display_buffer_count = available;
    display_buffer_index = 0;
    log_psram_state("display_buffers_ready");
    return ESP_OK;
}

static void trim_display_buffers_for_sleep(void)
{
    /*
     * Retain buffer 0 across display teardown so wake-up never needs to find
     * the first full-screen application framebuffer in a fragmented heap.
     * Release every secondary buffer only after DSI/LVGL is stopped. The BSP
     * is configured by the installer for two DPI framebuffers rather than its
     * memory-heavy triple-buffer default, leaving enough headroom to recreate
     * DSI while this one application buffer remains reserved.
     */
    for (int i = 1; i < APP_DISPLAY_BUFFER_COUNT; i++) {
        if (display_buffer[i] != NULL) {
            heap_caps_free(display_buffer[i]);
            display_buffer[i] = NULL;
        }
    }

    active_display_buffer_count = display_buffer[0] != NULL ? 1U : 0U;
    display_buffer_index = 0;
    log_psram_state("display_buffers_trimmed_for_sleep");
}

static esp_err_t face_boost_release(const char *reason)
{
    if (!system_cpu_face_boost_is_active()) {
        return ESP_OK;
    }

    esp_err_t ret = system_cpu_face_boost_end();
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not release face CPU boost (%s): %s",
            reason ? reason : "unspecified",
            esp_err_to_name(ret));
    }

    return ret;
}

static void power_manager_on_deep_sleep_state_requested(void)
{
    if (app_controller_handle_event(APP_EVENT_DEEP_SLEEP_REQUEST)) {
        ESP_LOGI(
            TAG,
            "[APP-STATE] -> DEEP_SLEEP");
    } else {
        ESP_LOGW(
            TAG,
            "[APP-STATE] DEEP_SLEEP_REQUEST did not cause a transition "
            "(current_state=%d)",
            (int)app_controller_get_state());
    }
}

static void power_manager_block_new_work(void)
{
    dummy_mode_delay_flag = true;
}


static bool power_manager_drain_active_work(uint32_t timeout_ms)
{
    return vision_ai_worker_state_pause_and_drain(timeout_ms);
}

static void power_manager_mark_backlight_off(void)
{
    /*
     * This hook runs before the destructive Deep-sleep sequence. In the
     * Light->Deep policy the backlight may intentionally still be ON while
     * the panel/DSI transport is already suspended, so request the physical
     * backlight OFF here rather than updating only the software shadow.
     */
    system_display_backlight_off();
    display_backlight_enabled = false;
    ESP_LOGI(
        TAG,
        "Deep-sleep transition: physical LCD backlight OFF requested");
}

static void power_manager_on_setup_error(
    power_manager_setup_stage_t stage,
    esp_err_t error)
{
    switch (stage) {
    case POWER_MANAGER_SETUP_LIGHT_SLEEP_CALLBACKS:
        ESP_LOGE(
            TAG,
            "Failed to register Light-sleep transitions: %s",
            esp_err_to_name(error));
        startup_error(
            "Light-sleep setup failed. Restart the device.");
        break;

    case POWER_MANAGER_SETUP_IDLE_SCAN_CALLBACKS:
        ESP_LOGE(
            TAG,
            "Failed to register adaptive IDLE-SCAN transitions: %s",
            esp_err_to_name(error));
        startup_error(
            "Adaptive CPU power setup failed. Restart the device.");
        break;

    case POWER_MANAGER_SETUP_BUTTON_MONITOR:
        ESP_LOGE(
            TAG,
            "Failed to start deep-sleep button monitor: %s",
            esp_err_to_name(error));
        startup_error(
            "Deep-sleep button task failed. Restart the device.");
        break;

    default:
        break;
    }
}

static bool idle_scan_display_is_suspended(void)
{
    bool suspended;
    portENTER_CRITICAL(&idle_scan_state_lock);
    suspended = display_suspended_for_idle_scan;
    portEXIT_CRITICAL(&idle_scan_state_lock);
    return suspended;
}

static void idle_scan_touch_poll_task(void *arg)
{
    (void)arg;

    /* Discard a coordinate consumed immediately before LVGL was stopped. */
    bool touched = false;
    (void)system_display_poll_touch_for_light_sleep(&touched);

    while (idle_scan_display_is_suspended() &&
           !display_suspended_for_light_sleep &&
           !power_manager_sleep_is_requested()) {
        vTaskDelay(pdMS_TO_TICKS(APP_LIGHT_SLEEP_TOUCH_POLL_MS));

        esp_err_t ret = system_display_poll_touch_for_light_sleep(&touched);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "IDLE-SCAN touch polling failed: %s",
                     esp_err_to_name(ret));
            break;
        }

        if (touched) {
            ESP_LOGI(TAG, "IDLE-SCAN touchscreen activity detected");
            power_manager_notify_activity();
            break;
        }
    }

    vTaskDelete(NULL);
}

static esp_err_t suspend_display_for_idle_scan(void *user_data)
{
    (void)user_data;

    if (video_cam_fd0 < 0 || display_mode_mutex == NULL || disp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(display_mode_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(TAG, "IDLE-SCAN timed out waiting for frame callback");
        return ESP_ERR_TIMEOUT;
    }

    if (idle_scan_display_is_suspended()) {
        xSemaphoreGive(display_mode_mutex);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "IDLE-SCAN suspending LVGL and MIPI-DSI; camera stays active");

    dummy_mode_delay_flag = true;
    system_display_backlight_off();
    display_backlight_enabled = false;

    esp_err_t ret = system_display_suspend_for_light_sleep(false);
    if (ret != ESP_OK) {
        xSemaphoreGive(display_mode_mutex);
        ESP_LOGE(TAG, "IDLE-SCAN display suspend failed: %s",
                 esp_err_to_name(ret));
        esp_restart();
        return ret;
    }

    /*
     * DSI is now stopped, so the application scanout buffers are no longer
     * owned by hardware. Free them before the later DSI recreation. This gives
     * the panel driver a large contiguous PSRAM block even with TFLM loaded.
     */
    trim_display_buffers_for_sleep();

    /* The BSP invalidates all LVGL display and input handles. */
    disp = NULL;
    launcher_touch_indev = NULL;
    dummy_draw_enabled = false;

    portENTER_CRITICAL(&idle_scan_state_lock);
    display_suspended_for_idle_scan = true;
    portEXIT_CRITICAL(&idle_scan_state_lock);

    /* Permit camera-only callbacks now that no DSI object can be accessed. */
    dummy_mode_delay_flag = false;

    authentication_session_reset();
    xSemaphoreGive(display_mode_mutex);

    BaseType_t created = xTaskCreatePinnedToCore(
        idle_scan_touch_poll_task,
        "idle_scan_touch",
        3072,
        NULL,
        4,
        NULL,
        APP_SYSTEM_WORKER_CORE);

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Could not create IDLE-SCAN touch polling task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "IDLE-SCAN display is off; camera-only scan is ready");
    return ESP_OK;
}

static void resume_display_from_idle_scan_task(void *arg)
{
    (void)arg;

    if (xSemaphoreTake(display_mode_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(TAG, "ACTIVE restore timed out waiting for frame callback");
        esp_restart();
    }

    if (!idle_scan_display_is_suspended() ||
        display_suspended_for_light_sleep ||
        power_manager_sleep_is_requested()) {
        portENTER_CRITICAL(&idle_scan_state_lock);
        idle_scan_resume_task_pending = false;
        portEXIT_CRITICAL(&idle_scan_state_lock);
        xSemaphoreGive(display_mode_mutex);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(
        TAG,
        "ACTIVE CPU policy restored; restoring MIPI-DSI and LVGL");

    dummy_mode_delay_flag = true;

    /*
     * The ESP32-P4 camera CSI and LCD DSI paths share DMA infrastructure.
     * Recreating MIPI-DSI while CSI is streaming can hand the LCD driver an
     * invalid GDMA device and cause a Store access fault. Pause CSI first,
     * restore the complete display stack, then restart the same camera task.
     */
    ESP_LOGI(TAG, "ACTIVE pausing camera DMA before MIPI-DSI restoration");
    esp_err_t ret = system_camera_stream_stop(video_cam_fd0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ACTIVE camera pause before display restore failed: %s",
                 esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        esp_restart();
    }

    log_psram_state("before_bsp_display_resume");
    disp = system_display_resume_from_light_sleep();
    if (disp == NULL) {
        ESP_LOGE(TAG, "ACTIVE display restoration failed");
        xSemaphoreGive(display_mode_mutex);
        esp_restart();
    }

    log_psram_state("after_bsp_display_resume");
    vTaskDelay(pdMS_TO_TICKS(150));
    launcher_touch_indev = system_display_get_input_device();

    ret = allocate_display_buffers();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ACTIVE display-buffer recreation failed: %s",
                 esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        esp_restart();
    }

    ret = esp_lv_adapter_set_dummy_draw(disp, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ACTIVE dummy-draw restoration failed: %s",
                 esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        esp_restart();
    }

    dummy_draw_enabled = true;
    display_buffer_index = 0;
    system_display_disable_lvgl_overlays(disp);
    vision_face_result_store_clear();

    ret = system_camera_stream_restart(video_cam_fd0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ACTIVE camera restart after display restore failed: %s",
                 esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        esp_restart();
    }

    diagnostics_ai_live_metrics_reset();
    diagnostics_cpu_hp_usage_reset();
    reset_preview_fps();
    s_last_preview_start_us = -1;
    s_last_ai_submit_us = -1;

    portENTER_CRITICAL(&idle_scan_state_lock);
    display_suspended_for_idle_scan = false;
    idle_scan_resume_task_pending = false;
    portEXIT_CRITICAL(&idle_scan_state_lock);

    /* The first complete frame turns the physical backlight on. */
    display_backlight_enabled = false;
    dummy_mode_delay_flag = false;

    xSemaphoreGive(display_mode_mutex);
    ESP_LOGI(
        TAG,
        "ACTIVE camera display restored; baseline=%d MHz boost=%s",
        APP_CPU_ACTIVE_FREQ_MHZ,
        system_cpu_face_boost_is_active() ? "on" : "off");
    vTaskDelete(NULL);
}

static esp_err_t request_display_resume_from_idle_scan(void *user_data)
{
    (void)user_data;

    if (!idle_scan_display_is_suspended()) {
        return ESP_OK;
    }

    portENTER_CRITICAL(&idle_scan_state_lock);
    if (idle_scan_resume_task_pending) {
        portEXIT_CRITICAL(&idle_scan_state_lock);
        return ESP_OK;
    }
    idle_scan_resume_task_pending = true;
    portEXIT_CRITICAL(&idle_scan_state_lock);

    BaseType_t created = xTaskCreatePinnedToCore(
        resume_display_from_idle_scan_task,
        "idle_scan_resume",
        6144,
        NULL,
        7,
        NULL,
        APP_SYSTEM_WORKER_CORE);

    if (created != pdPASS) {
        portENTER_CRITICAL(&idle_scan_state_lock);
        idle_scan_resume_task_pending = false;
        portEXIT_CRITICAL(&idle_scan_state_lock);
        ESP_LOGE(TAG, "Could not create ACTIVE display-resume task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t suspend_application_for_light_sleep(void *user_data)
{
    (void)user_data;

    if (video_cam_fd0 < 0 || display_mode_mutex == NULL) {
        ESP_LOGE(TAG, "Camera/display are not ready for Light-sleep suspend");
        return ESP_ERR_INVALID_STATE;
    }

    if (!vision_ai_worker_state_pause_and_drain(APP_AI_WORKER_DRAIN_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Timed out draining AI worker before Light-sleep");
        return ESP_ERR_TIMEOUT;
    }

    /*
     * The worker may have found a face while this callback waited for a long
     * FP32 inference to finish. In that case power_manager_notify_activity()
     * reset the inactivity timer, so cancel the claimed Light-sleep before any
     * display/camera hardware is destroyed.
     */
    if (!power_manager_light_sleep_is_due()) {
        vision_ai_worker_state_resume_accepting();
        ESP_LOGI(TAG, "Light-sleep canceled because AI/user activity arrived");
        return ESP_ERR_INVALID_STATE;
    }

    face_boost_release("Light-sleep suspend");

    ESP_LOGI(TAG, "Suspending camera and MIPI-DSI for Light-sleep");

    /*
     * Establish an exclusive display barrier before asking the video task to
     * exit. A frame callback already in progress is allowed to finish; after
     * this mutex is acquired, every new callback fails its non-blocking mutex
     * attempt and returns immediately. This removes the stop-vs-display race
     * that previously caused the video-stop semaphore to time out.
     */
    if (xSemaphoreTake(
            display_mode_mutex,
            pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(
            TAG,
            "Timed out waiting for active frame callback before Light-sleep");
        return ESP_ERR_TIMEOUT;
    }

    dummy_mode_delay_flag = true;
    ESP_LOGI(TAG, "Light-sleep display barrier acquired; stopping camera");

    esp_err_t ret = system_camera_stream_stop(video_cam_fd0);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Camera stream did not stop for Light-sleep: %s",
            esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        return ret;
    }

    ret = system_camera_prepare_sensor_for_light_sleep();
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "OV5647 sensor standby failed before Light-sleep: %s",
            esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        return ret;
    }

    display_backlight_enabled = false;

    if (idle_scan_display_is_suspended()) {
        /* MIPI-DSI is already off; Light-sleep only had to stop the camera. */
        ret = ESP_OK;
        portENTER_CRITICAL(&idle_scan_state_lock);
        display_suspended_for_idle_scan = false;
        portEXIT_CRITICAL(&idle_scan_state_lock);
        ESP_LOGI(TAG, "IDLE-SCAN display already suspended for Light-sleep");
    } else {
        if (disp == NULL) {
            xSemaphoreGive(display_mode_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        /*
         * V15 CONSISTENCY:
         * Light-only and Light->Deep must reach the exact same electrical
         * Light-sleep state. Always commit LCD DISPLAY_OFF + SLEEP_IN while the
         * panel handle is alive, then tear down MIPI-DSI/LVGL. Deep enablement
         * changes only what happens AFTER the Light-sleep residency window.
         */
        ret = system_display_suspend_for_light_sleep(true);
        if (ret == ESP_OK) {
            trim_display_buffers_for_sleep();
        }
    }

#if APP_LIGHT_SLEEP_KEEP_BACKLIGHT_ON
    if (ret == ESP_OK) {
        /*
         * Deliberately re-enable only the physical backlight AFTER the panel
         * accepted SLEEP_IN and MIPI-DSI/LVGL were suspended. No framebuffer,
         * panel scanout, camera stream or AI work is restarted here.
         */
        system_display_backlight_on();
        display_backlight_enabled = true;
        ESP_LOGI(
            TAG,
            "Light-sleep visual standby: backlight ON; "
            "JD9365/MIPI-DSI/camera remain suspended");
    }
#endif

    /* The adapter invalidates every LVGL display/input object on shutdown. */
    disp = NULL;
    launcher_touch_indev = NULL;
    dummy_draw_enabled = false;
    display_suspended_for_light_sleep = true;

    authentication_session_reset();
    xSemaphoreGive(display_mode_mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Display suspend completed with errors: %s",
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * Application-owned camera/display resources are now genuinely
     * suspended. The power service will continue with auxiliary
     * peripheral suspension and the actual Light-sleep call.
     *
     *     CAMERA_ACTIVE/PIN_ENTRY -> LIGHT_SLEEP
     */
    if (app_controller_handle_event(APP_EVENT_LIGHT_SLEEP_REQUEST)) {
        ESP_LOGI(
            TAG,
            "[APP-STATE] -> LIGHT_SLEEP");
    } else {
        ESP_LOGW(
            TAG,
            "[APP-STATE] LIGHT_SLEEP_REQUEST did not cause a transition "
            "(current_state=%d)",
            (int)app_controller_get_state());
    }

    ESP_LOGI(TAG, "Camera and MIPI-DSI suspended for Light-sleep");
    return ESP_OK;
}

static esp_err_t resume_application_from_light_sleep(void *user_data)
{
    (void)user_data;

    const int64_t light_wake_start_us =
        app_sleep_take_light_wake_start_us();
    wake_timing_arm(WAKE_TIMING_LIGHT_SLEEP, light_wake_start_us);

    if (!display_suspended_for_light_sleep || video_cam_fd0 < 0 ||
        display_mode_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(
            display_mode_mutex,
            pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting to restore display after Light-sleep");
        return ESP_ERR_TIMEOUT;
    }

#if APP_LIGHT_SLEEP_KEEP_BACKLIGHT_ON
    /*
     * Remove the standby illumination while reconstructing the display.
     * The normal first complete camera frame will turn the backlight on again,
     * preserving the existing tear-free wake behavior.
     */
    system_display_backlight_off();
    display_backlight_enabled = false;
    ESP_LOGI(
        TAG,
        "Leaving Light-sleep visual standby: backlight OFF during display restore");
#endif

    ESP_LOGI(TAG, "Restoring MIPI-DSI and camera after Light-sleep activity");

    log_psram_state("before_bsp_display_resume");
    disp = system_display_resume_from_light_sleep();
    if (disp == NULL) {
        ESP_LOGE(TAG, "Display reinitialization after Light-sleep failed");
        xSemaphoreGive(display_mode_mutex);
        return ESP_FAIL;
    }

    log_psram_state("after_bsp_display_resume");
    vTaskDelay(pdMS_TO_TICKS(150));
    launcher_touch_indev = system_display_get_input_device();

    esp_err_t ret = allocate_display_buffers();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display-buffer recreation after Light-sleep failed: %s",
                 esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        return ret;
    }

    ret = esp_lv_adapter_set_dummy_draw(disp, true);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not restore direct camera display mode: %s",
            esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        return ret;
    }

    dummy_draw_enabled = true;
    display_buffer_index = 0;
    system_display_disable_lvgl_overlays(disp);

    ret = system_camera_restore_sensor_after_light_sleep();
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "OV5647 sensor restore after Light-sleep failed: %s",
            esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        return ret;
    }

    ret = system_camera_stream_restart(video_cam_fd0);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Camera stream restart after Light-sleep failed: %s",
            esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        return ret;
    }

    diagnostics_ai_live_metrics_reset();
    diagnostics_cpu_hp_usage_reset();
    reset_preview_fps();
    s_last_preview_start_us = -1;
    s_last_ai_submit_us = -1;

    /*
     * The complete display stack and camera stream are live again. Restore the
     * physical backlight here instead of depending on a later frame callback;
     * this guarantees that a touchscreen Light-sleep wake becomes visibly ACTIVE.
     */
    system_display_backlight_on();
    display_backlight_enabled = true;
    display_suspended_for_light_sleep = false;
    dummy_mode_delay_flag = false;
    vision_face_result_store_clear();
    vision_ai_worker_state_resume_accepting();

    xSemaphoreGive(display_mode_mutex);

    /* Light-sleep restores generic peripherals; reassert saved OFF policies. */
    ret = app_configuration_apply_saved_policy();
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not reapply saved power policy after Light-sleep: %s",
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * Display, camera sensor, video stream, AI worker and saved
     * component policy have all been restored successfully.
     *
     *     LIGHT_SLEEP -> CAMERA_ACTIVE
     */
    if (app_controller_handle_event(APP_EVENT_WAKE)) {
        ESP_LOGI(
            TAG,
            "[APP-STATE] LIGHT_SLEEP -> CAMERA_ACTIVE");
    } else {
        ESP_LOGW(
            TAG,
            "[APP-STATE] WAKE did not cause a transition "
            "(current_state=%d)",
            (int)app_controller_get_state());
    }

    ESP_LOGI(TAG, "Camera and display restored after Light-sleep");
    return ESP_OK;
}

static void camera_application_start_task(void *arg)
{
    (void)arg;

    esp_err_t ret;

    /* Re-apply saved component states immediately before Active camera mode. */
    ret = app_configuration_apply_saved_policy();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Saved power policy could not be applied: %s",
                 esp_err_to_name(ret));
        startup_error(
            "Power setting could not be applied. Check the serial log and restart.");
        application_start_requested = false;
        vTaskDelete(NULL);
        return;
    }

    const app_configuration_snapshot_t settings = app_configuration_get();
    if (!settings.camera_enabled) {
        ESP_LOGW(TAG, "Camera start blocked by the saved Camera OFF policy");
        startup_error("Camera is disabled. Enable it in Settings first.");
        application_start_requested = false;
        vTaskDelete(NULL);
        return;
    }

    startup_status("Initializing display accelerator...");

    ppa_client_config_t ppa_srm_config = {
        .oper_type = PPA_OPERATION_SRM,
    };

    ret = ppa_register_client(&ppa_srm_config, &ppa_srm_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA client registration failed: %s", esp_err_to_name(ret));
        startup_error("PPA initialization failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    startup_status("Initializing face detection and recognition...");

    /*
     * Models, enrollment images and the recognition database are loaded from
     * microSD during service initialization. If the saved policy is OFF, grant
     * only this bounded startup access and remove SD1_VDD again before Active
     * camera capture begins.
     */
    const bool restore_sdcard_off_after_ai_init = !settings.sdcard_enabled;
    if (restore_sdcard_off_after_ai_init) {
        ret = power_manager_set_sdcard_enabled(true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Temporary microSD startup access failed: %s",
                     esp_err_to_name(ret));
            startup_error(
                "microSD could not be powered for AI initialization.");
            application_start_requested = false;
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "microSD temporarily enabled for AI/model initialization");
    }

    /*
     * Keep the existing service initialization unchanged, but defer it until
     * the user presses Start so the launcher GUI appears immediately at boot.
     */
    app_boot_initialize_services();

    /*
     * Start the camera-screen telemetry from a clean epoch.  Startup model
     * loading/enrollment must never appear as live inference latency.
     */
    diagnostics_ai_live_metrics_reset();
    diagnostics_cpu_hp_usage_reset();
    reset_preview_fps();
    frame_count = 0;
    s_last_preview_start_us = -1;
    s_last_ai_submit_us = -1;

    if (restore_sdcard_off_after_ai_init) {
        ret = power_manager_set_sdcard_enabled(false);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Could not restore saved microSD OFF state: %s",
                     esp_err_to_name(ret));
            startup_error(
                "AI loaded, but microSD could not be powered down safely.");
            application_start_requested = false;
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "microSD returned to saved OFF state before Active camera mode");
    }

#if APP_DIAGNOSTICS_AI_PIPELINE_ENABLED
    diagnostics_start_ai_pipeline_monitor();
#endif

    ret = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cache alignment query failed: %s", esp_err_to_name(ret));
        startup_error("Memory initialization failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    lcd_fb_size = ALIGN_UP(
        (size_t)system_display_width() * system_display_height() *
            system_camera_bytes_per_pixel(),
        data_cache_line_size);

    startup_status("Opening camera...");

    ret = system_camera_initialize();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Video initialization failed: %s", esp_err_to_name(ret));
        startup_error("Camera initialization failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    video_cam_fd0 = system_camera_open_default();
    if (video_cam_fd0 < 0) {
        ESP_LOGE(TAG, "Video camera open failed");
        startup_error("Camera could not be opened. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    startup_status("Allocating camera buffers...");

    ret = allocate_display_buffers();
    if (ret != ESP_OK) {
        startup_error("Display-buffer allocation failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Using independent buffers, display_buf_active=%u display_buf_max=%d camera_buf=%d",
             (unsigned)active_display_buffer_count,
             APP_DISPLAY_BUFFER_COUNT,
             APP_CAMERA_BUFFER_COUNT);

    void *camera_buf[APP_CAMERA_BUFFER_COUNT];
    for (int i = 0; i < APP_CAMERA_BUFFER_COUNT; i++) {
        camera_buf[i] = heap_caps_aligned_calloc(
            data_cache_line_size,
            1,
            system_camera_buffer_size(),
            MALLOC_CAP_SPIRAM);

        if (!camera_buf[i]) {
            ESP_LOGE(TAG, "Failed to allocate camera buffer %d", i);
            startup_error("Camera-buffer allocation failed. Restart the device.");
            vTaskDelete(NULL);
            return;
        }
    }

    ret = system_camera_set_buffers(
        video_cam_fd0,
        APP_CAMERA_BUFFER_COUNT,
        (const void **)camera_buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera buffer setup failed: %s", esp_err_to_name(ret));
        startup_error("Camera-buffer setup failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    if (vision_ai_snapshot_buffer_init(data_cache_line_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate %u-byte AI snapshot buffer",
                 (unsigned)vision_ai_snapshot_buffer_capacity());
        startup_error("AI snapshot allocation failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    if (vision_ai_inference_guard_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create AI inference mutex");
        startup_error("AI synchronization failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    const authentication_flow_context_t authentication_flow_context = {
        .display_mode_mutex = &display_mode_mutex,
        .display = &disp,
        .video_fd = &video_cam_fd0,
        .display_buffer_index = &display_buffer_index,
        .dummy_draw_enabled = &dummy_draw_enabled,
        .release_face_boost = face_boost_release,
    };
    authentication_flow_initialize(&authentication_flow_context);

    const vision_ai_worker_pipeline_hooks_t ai_worker_hooks = {
        .idle_scan_display_is_suspended = idle_scan_display_is_suspended,
        .sleep_is_requested = power_manager_sleep_is_requested,
        .camera_processing_blocked = authentication_session_blocks_camera,
        .note_detection_result = authentication_session_note_detection_result,
        .acquire_face_boost = system_cpu_face_boost_begin,
        .notify_activity = power_manager_notify_activity,
        .recognition_started =
            authentication_flow_on_recognition_started,
        .request_pin = authentication_flow_request_pin,
        .recognition_not_authenticated =
            authentication_flow_on_recognition_not_authenticated,
        .launch_pin_transition =
            authentication_flow_launch_pin_transition,
        .release_face_boost = face_boost_release,
    };

    bool ai_created =
        vision_ai_worker_pipeline_start(&ai_worker_hooks);

    if (!ai_created) {
        ESP_LOGE(TAG, "Failed to create asynchronous AI worker");
        startup_error("AI worker creation failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    vision_ai_worker_state_resume_accepting();
    ESP_LOGI(TAG,
             "Async AI ready: core=%d snapshot_max=%ux%u bytes=%u",
             APP_AI_WORKER_CORE,
             (unsigned)APP_AI_SNAPSHOT_MAX_EDGE,
             (unsigned)APP_AI_SNAPSHOT_MAX_EDGE,
             (unsigned)vision_ai_snapshot_buffer_capacity());

    ret = diagnostics_cpu_hp_usage_start();
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not start live HP Core 0/Core 1 usage telemetry: %s",
            esp_err_to_name(ret));
        startup_error(
            "CPU usage telemetry could not start. Check FreeRTOS run-time stats.");
        application_start_requested = false;
        vTaskDelete(NULL);
        return;
    }

    diagnostics_cpu_hp_usage_reset();

    display_mode_mutex = xSemaphoreCreateMutex();
    if (!display_mode_mutex) {
        ESP_LOGE(TAG, "Failed to create display-mode mutex");
        startup_error("Display synchronization failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    ret = system_camera_register_frame_callback(camera_video_frame_operation);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Frame callback registration failed: %s", esp_err_to_name(ret));
        startup_error("Camera callback setup failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    startup_status("Starting live application...");

    /*
     * Start capture while dummy draw is still disabled. Any frame arriving in
     * this tiny transition window is safely ignored by the callback.
     */
    ESP_LOGI(TAG,
             "[CORE-PROOF] Requesting video stream task on system CPU%d",
             APP_SYSTEM_WORKER_CORE);
    ret = system_camera_stream_start(
        video_cam_fd0, APP_SYSTEM_WORKER_CORE, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Video stream task failed: %s", esp_err_to_name(ret));
        startup_error("Camera stream failed to start. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    /* Hide the launcher before granting direct display ownership to camera. */
    system_display_backlight_off();
    display_backlight_enabled = false;

    /*
     * Remove every launcher object while LVGL still owns the display. Dummy
     * draw is enabled only after the UI has been destroyed safely.
     */
    if (!s_direct_deep_wake_start) {
        presentation_launcher_destroy();
        vTaskDelay(pdMS_TO_TICKS(50));
    } else {
        ESP_LOGI(
            TAG,
            "[DEEP-WAKE] launcher destroy skipped because launcher was never created");
    }

    ret = esp_lv_adapter_set_dummy_draw(disp, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not enable camera display mode: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    dummy_draw_enabled = true;
    system_display_disable_lvgl_overlays(disp);

    /* Start the 30-second face-inactivity window with the camera application. */
    ret = power_manager_start_inactivity_policy();
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to start face-inactivity monitor: %s",
            esp_err_to_name(ret));
    }

    /*
     * Camera, AI worker and live display path are now initialized.
     *
     *     LAUNCHER -> CAMERA_ACTIVE
     *
     * The event is intentionally dispatched only here, after successful
     * camera startup. If initialization fails earlier, the controller
     * remains in LAUNCHER.
     */
    if (app_controller_handle_event(APP_EVENT_START_CAMERA)) {
        ESP_LOGI(TAG, "[APP-STATE] LAUNCHER -> CAMERA_ACTIVE%s",
                 s_direct_deep_wake_start ? " (deep-wake UI bypass)" : "");
    } else {
        ESP_LOGW(
            TAG,
            "[APP-STATE] START_CAMERA did not cause a transition");
    }

    ESP_LOGI(
        TAG,
        "%s",
        s_direct_deep_wake_start
            ? "[DEEP-WAKE] direct camera application startup complete"
            : "Launcher completed; camera application is running");
    vTaskDelete(NULL);
}

void camera_session_launcher_start_requested(void *user_data)
{
    (void)user_data;

    ESP_LOGI(
        TAG,
        "%s",
        s_direct_deep_wake_start
            ? "[DEEP-WAKE] direct camera start request received"
            : "Launcher start callback received");

    if (application_start_requested) {
        return;
    }

    application_start_requested = true;

    BaseType_t created = xTaskCreatePinnedToCore(
        camera_application_start_task,
        "camera_app_start",
        8192,
        NULL,
        6,
        NULL,
        APP_SYSTEM_WORKER_CORE);

    if (created != pdPASS) {
        application_start_requested = false;
        ESP_LOGE(TAG, "Failed to create camera startup task");
        startup_error("Could not start application task. Restart the device.");
    }
}



void camera_session_start_direct_after_deep_sleep(int64_t deep_wake_start_us)
{
    s_direct_deep_wake_start = true;
    wake_timing_arm(WAKE_TIMING_DEEP_SLEEP, deep_wake_start_us);

    ESP_LOGI(
        TAG,
        "[DEEP-WAKE] GPIO3 wake fast path: starting camera directly; "
        "launcher UI will not be created");

    camera_session_launcher_start_requested(NULL);
}

esp_err_t camera_session_setup_power_management(void)
{
    const power_manager_hooks_t power_manager_hooks = {
        .light_sleep_suspend = suspend_application_for_light_sleep,
        .light_sleep_resume = resume_application_from_light_sleep,
        .idle_scan_suspend = suspend_display_for_idle_scan,
        .idle_scan_resume = request_display_resume_from_idle_scan,
        .deep_sleep_state_requested =
            power_manager_on_deep_sleep_state_requested,
        .block_new_work = power_manager_block_new_work,
        .drain_active_work = power_manager_drain_active_work,
        .release_face_boost = face_boost_release,
        .mark_backlight_off = power_manager_mark_backlight_off,
        .setup_error = power_manager_on_setup_error,
    };

    return power_manager_setup(&power_manager_hooks);
}

static void camera_video_frame_operation(
    uint8_t *camera_buf,
    uint8_t camera_buf_index,
    uint32_t camera_buf_hes,
    uint32_t camera_buf_ves,
    size_t camera_buf_len,
    void *user_data)
{
    if (!display_mode_mutex ||
        xSemaphoreTake(display_mode_mutex, 0) != pdTRUE) {
        return;
    }

    camera_video_frame_process(
        camera_buf,
        camera_buf_index,
        camera_buf_hes,
        camera_buf_ves,
        camera_buf_len,
        user_data);

    xSemaphoreGive(display_mode_mutex);
}

static void camera_video_frame_process(
    uint8_t *camera_buf,
    uint8_t camera_buf_index,
    uint32_t camera_buf_hes,
    uint32_t camera_buf_ves,
    size_t camera_buf_len,
    void *user_data)
{
    const bool idle_scan_frame = idle_scan_display_is_suspended();

    if ((!dummy_draw_enabled && !idle_scan_frame) ||
        (dummy_mode_delay_flag && !idle_scan_frame) ||
        authentication_session_blocks_camera()) {
        return;
    }

    (void)camera_buf_index;
    (void)user_data;

    const uint32_t display_width = system_display_width();
    const uint32_t display_height = system_display_height();
    if (display_width == 0 || display_height == 0) {
        ESP_LOGE(TAG, "Display dimensions are invalid");
        return;
    }

    void *target_fb = NULL;
    esp_err_t ret = ESP_OK;
    const int64_t frame_start_us = esp_timer_get_time();
    const bool preview_due = !display_backlight_enabled ||
        work_interval_elapsed(frame_start_us, s_last_preview_start_us,
                              system_cpu_preview_min_interval_ms());

    /*
     * DISPLAY FIRST.
     *
     * The old implementation ran BlazeFace/MobileFaceNet before this block,
     * freezing the LCD for every inference. The video task now performs only
     * the PPA scale, overlays the most recently completed AI result, submits
     * the frame, and returns. AI runs independently on CPU1.
     */
    if (!idle_scan_frame && preview_due) {
        if (active_display_buffer_count == 0) {
            ESP_LOGE(TAG, "No active display buffer");
            return;
        }
        if (display_buffer_index >= active_display_buffer_count) {
            display_buffer_index = 0;
        }
        target_fb = display_buffer[display_buffer_index];
        if (target_fb == NULL) {
            ESP_LOGE(TAG, "display buffer is NULL");
            return;
        }

        const float scale_x = (float)display_width / (float)camera_buf_hes;
        const float scale_y = (float)display_height / (float)camera_buf_ves;

        ppa_srm_oper_config_t srm_config = {
            .in.buffer = camera_buf,
            .in.pic_w = camera_buf_hes,
            .in.pic_h = camera_buf_ves,
            .in.block_w = camera_buf_hes,
            .in.block_h = camera_buf_ves,
            .in.block_offset_x = 0,
            .in.block_offset_y = 0,
            .in.srm_cm = system_camera_is_rgb565()
                ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,

            .out.buffer = target_fb,
            .out.buffer_size = lcd_fb_size,
            .out.pic_w = display_width,
            .out.pic_h = display_height,
            .out.block_offset_x = 0,
            .out.block_offset_y = 0,
            .out.srm_cm = system_camera_is_rgb565()
                ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,

            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = scale_x,
            .scale_y = scale_y,
            .mirror_x = 0,
            .mirror_y = 0,
            .rgb_swap = 0,
            .byte_swap = 0,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };

        ret = ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PPA SRM failed: %d", ret);
            return;
        }

        if (system_camera_is_rgb565()) {
            /*
             * PPA wrote target_fb through hardware. Invalidate once before all
             * CPU-side overlays, then write back once after the face overlay
             * and telemetry panel are complete.
             */
#if APP_SYNC_CACHE_AROUND_OVERLAY
            esp_cache_msync(
                target_fb,
                lcd_fb_size,
                ESP_CACHE_MSYNC_FLAG_DIR_M2C);
#endif

            face_box_t overlay_boxes[APP_MAX_FACE_BOXES] = {0};
            char overlay_names[APP_MAX_FACE_BOXES][FACE_RECOG_MAX_NAME_LEN] = {{0}};

            const int overlay_count =
                vision_face_result_store_snapshot(
                    overlay_boxes,
                    overlay_names,
                    NULL,
                    APP_MAX_FACE_BOXES);

            for (int i = 0; i < overlay_count; i++) {
                const int lcd_x1 =
                    overlay_boxes[i].x1 * display_width / camera_buf_hes;
                const int lcd_y1 =
                    overlay_boxes[i].y1 * display_height / camera_buf_ves;
                const int lcd_x2 =
                    overlay_boxes[i].x2 * display_width / camera_buf_hes;
                const int lcd_y2 =
                    overlay_boxes[i].y2 * display_height / camera_buf_ves;

                const bool recognized =
                    overlay_names[i][0] != '\0' &&
                    strcmp(overlay_names[i], "unknown") != 0;
                const uint16_t overlay_color =
                    recognized ? 0x07E0 : 0xF800;

                presentation_face_overlay_draw_box_rgb565(
                    (uint16_t *)target_fb,
                    display_width,
                    display_height,
                    lcd_x1,
                    lcd_y1,
                    lcd_x2,
                    lcd_y2,
                    APP_FACE_BOX_THICKNESS,
                    overlay_color);

                presentation_face_overlay_draw_label_rgb565(
                    (uint16_t *)target_fb,
                    display_width,
                    display_height,
                    lcd_x2,
                    lcd_y1,
                    overlay_names[i],
                    overlay_color);
            }

            diagnostics_ai_live_metrics_t live_metrics = {0};
            diagnostics_ai_live_metrics_snapshot(&live_metrics);

            uint32_t hp_core0_usage_x10 = 0;
            uint32_t hp_core1_usage_x10 = 0;

            const bool hp_cpu_usage_valid =
                diagnostics_cpu_hp_usage_snapshot(
                    &hp_core0_usage_x10,
                    &hp_core1_usage_x10);

            presentation_camera_metrics_draw_rgb565(
                (uint16_t *)target_fb,
                display_width,
                display_height,
                live_metrics.detector_valid,
                live_metrics.detector_inference_us,
                live_metrics.recognizer_valid,
                live_metrics.recognizer_inference_us,
                s_preview_fps_x10,
                hp_cpu_usage_valid,
                hp_core0_usage_x10,
                hp_core1_usage_x10);

#if APP_SYNC_CACHE_AROUND_OVERLAY
            esp_cache_msync(
                target_fb,
                lcd_fb_size,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#endif
        }

        if (dummy_draw_enabled && !dummy_mode_delay_flag) {
            ret = esp_lv_adapter_dummy_draw_blit(
                disp,
                0,
                0,
                display_width,
                display_height,
                target_fb,
                true);

            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Dummy draw blit failed: %d", ret);
                return;
            }

            display_buffer_index =
                (display_buffer_index + 1) % active_display_buffer_count;
            s_last_preview_start_us = frame_start_us;
            note_preview_frame_presented(esp_timer_get_time());

            /* First frame, real activity and idle stages all use the same
             * brightness adapter. It caches successful values, so this does
             * not create an I2C transaction on every rendered frame. */
            if (system_display_backlight_set_percent(
                    system_cpu_backlight_percent()) == ESP_OK) {
                display_backlight_enabled = true;
                wake_timing_finish_on_first_camera_frame();
            }
        }
    }

    frame_count++;

    if ((frame_count % 100U) == 0U) {
        ESP_LOGD(TAG,
                 "[CORE-PROOF] DISPLAY frame=%" PRIu32
                 " cpu=%d task=%s affinity=%d",
                 frame_count,
                 xPortGetCoreID(),
                 pcTaskGetName(NULL),
                 xTaskGetCoreID(xTaskGetCurrentTaskHandle()));
    }

    /*
     * Schedule only the latest frame when the worker is idle. The snapshot copy
     * is a small nearest-neighbour downscale (<= APP_AI_SNAPSHOT_MAX_EDGE), so
     * the CPU0 camera task spends milliseconds here instead of seconds.
     */
    const uint32_t detect_interval_frames =
        system_cpu_face_detect_interval_frames();

    /* Rendering can be skipped while camera buffers continue to circulate.
     * AI scheduling stays outside the preview gate and still uses the newest
     * camera buffer. Failed/busy submissions do not consume the time budget. */
    const int64_t submit_us = esp_timer_get_time();
    if (detect_interval_frames > 0 &&
        (frame_count % detect_interval_frames) == 0U &&
        work_interval_elapsed(submit_us, s_last_ai_submit_us,
                              system_cpu_ai_min_interval_ms())) {
        if (vision_ai_snapshot_scheduler_schedule(
            camera_buf,
            camera_buf_len,
            camera_buf_hes,
            camera_buf_ves,
            frame_count,
            idle_scan_frame)) {
            s_last_ai_submit_us = submit_us;
        }
    }
}
