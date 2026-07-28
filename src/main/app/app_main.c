#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_video_init.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "platform/camera/video_capture.h"
#include "services/vision/face_detector.h"
#include "services/vision/face_recognizer.h"
#include "diagnostics/core_trace.h"
#include "diagnostics/cpu_stats.h"
#include "diagnostics/ai_pipeline_status.h"
#include "app/app_boot.h"
#include "app_ui.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "lv_demos.h"
#include "power_save/deep_sleep.h"
#include "power_save/wake_up.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "power_save/cpu_power.h"

#define DEEP_SLEEP_TIMEOUT_MS 30000
#define DEEP_SLEEP_BUTTON_GPIO GPIO_NUM_3
#define DEEP_SLEEP_BUTTON_POLL_MS 5
#define DEEP_SLEEP_BUTTON_DEBOUNCE_MS 25
#define DEEP_SLEEP_BUTTON_TASK_PRIORITY 8
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
#define CAMERA_BUF_NUM 2
#define DISPLAY_BUF_NUM 2

/*
 * The PPA writes the display buffer through hardware, while the face rectangle
 * is written by the CPU. Invalidate before the CPU touches a PPA-written buffer,
 * then write it back before the LCD reads it.
 */
#define SYNC_CACHE_AROUND_OVERLAY 1

static int smooth_coord(int old_value, int new_value)
{
    return (old_value * 3 + new_value) / 4;
}

static void draw_rect_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int x1,
    int y1,
    int x2,
    int y2,
    uint16_t color)
{
    if (!fb) {
        return;
    }

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 < 0) x2 = 0;
    if (y2 < 0) y2 = 0;

    if (x1 >= (int)fb_w) x1 = fb_w - 1;
    if (x2 >= (int)fb_w) x2 = fb_w - 1;
    if (y1 >= (int)fb_h) y1 = fb_h - 1;
    if (y2 >= (int)fb_h) y2 = fb_h - 1;

    for (int x = x1; x <= x2; x++) {
        fb[y1 * fb_w + x] = color;
        fb[y2 * fb_w + x] = color;
    }

    for (int y = y1; y <= y2; y++) {
        fb[y * fb_w + x1] = color;
        fb[y * fb_w + x2] = color;
    }
}

static void camera_video_frame_operation(
    uint8_t *camera_buf,
    uint8_t camera_buf_index,
    uint32_t camera_buf_hes,
    uint32_t camera_buf_ves,
    size_t camera_buf_len,
    void *user_data);

static const char *TAG = "app_main";

static ppa_client_handle_t ppa_srm_handle = NULL;
static size_t data_cache_line_size = 0;
static void *display_buffer[DISPLAY_BUF_NUM];
static size_t lcd_fb_size = 0;
static uint8_t display_buffer_index = 0;
static bool display_backlight_enabled = false;
static portMUX_TYPE deep_sleep_request_lock = portMUX_INITIALIZER_UNLOCKED;
static bool deep_sleep_requested = false;
static lv_display_t *disp;
static lv_indev_t *launcher_touch_indev = NULL;
#define FACE_DETECT_INTERVAL 5
#define MAX_FACE_BOXES 5
#define FACE_RECOG_INTERVAL 20

static uint32_t frame_count = 0;

static face_box_t last_boxes[MAX_FACE_BOXES];
static int last_face_count = 0;
static int no_face_frames = 0;

#define FACE_BOX_HOLD_MISSES 3

i2c_master_bus_handle_t i2c_bus_;

static bool dummy_draw_enabled = false;
static bool application_start_requested = false;
static bool dummy_mode_delay_flag = false;

/*
 * Remove LVGL's performance and memory labels before the camera preview is
 * shown. Dummy draw mode then keeps LVGL from rendering over the video path.
 *
 * Keep these disabled in sdkconfig as well:
 *     CONFIG_LV_USE_PERF_MONITOR=n
 *     CONFIG_LV_USE_MEM_MONITOR=n
 */
static void display_disable_lvgl_overlays(void)
{
#if LV_USE_SYSMON
    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "Could not lock LVGL to hide system overlays");
        return;
    }

#if LV_USE_PERF_MONITOR && LV_VERSION_CHECK(9, 2, 0)
    lv_sysmon_hide_performance(disp);
#endif

#if LV_USE_MEM_MONITOR && LV_VERSION_CHECK(9, 2, 0)
    lv_sysmon_hide_memory(disp);
#endif

    bsp_display_unlock();
#endif
}

static bool claim_deep_sleep_request(void)
{
    bool claimed = false;

    portENTER_CRITICAL(&deep_sleep_request_lock);
    if (!deep_sleep_requested) {
        deep_sleep_requested = true;
        claimed = true;
    }
    portEXIT_CRITICAL(&deep_sleep_request_lock);

    return claimed;
}

static void request_deep_sleep(const char *reason)
{
    if (!claim_deep_sleep_request()) {
        return;
    }

    ESP_LOGI(TAG, "Deep sleep requested by %s", reason);

    /*
     * Stop the camera callback from doing more PPA, face detection,
     * recognition or LCD operations while the camera is shutting down.
     */
    dummy_mode_delay_flag = true;

    /*
     * Turn off the display backlight immediately.
     */
    bsp_display_backlight_off();
    display_backlight_enabled = false;

    /*
     * Stop the V4L2 stream task, execute VIDIOC_STREAMOFF,
     * and close the camera file descriptor.
     *
     * This is also safe when the camera has not been started yet.
     */
    esp_err_t camera_ret = app_video_shutdown();

    if (camera_ret == ESP_OK) {
        ESP_LOGI(TAG, "Camera shut down successfully");
    } else {
        ESP_LOGW(
            TAG,
            "Camera shutdown failed: %s",
            esp_err_to_name(camera_ret));
    }

    /*
     * Configure the GPIO3 wake source and enter deep sleep.
     */
    enter_deep_sleep();

    /*
     * Normally unreachable. This executes only if deep sleep failed.
     */
    dummy_mode_delay_flag = false;

    portENTER_CRITICAL(&deep_sleep_request_lock);
    deep_sleep_requested = false;
    portEXIT_CRITICAL(&deep_sleep_request_lock);

    ESP_LOGE(TAG, "Deep sleep request returned without entering sleep");
}

static void deep_sleep_button_task(void *arg)
{
    (void)arg;

    gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << DEEP_SLEEP_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&button_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure deep-sleep button: %s",
                 esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    /*
     * A wake-up press may still be held while the application boots. Wait for
     * its release so the same press cannot immediately put the device to sleep.
     */
    while (gpio_get_level(DEEP_SLEEP_BUTTON_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(DEEP_SLEEP_BUTTON_POLL_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(DEEP_SLEEP_BUTTON_DEBOUNCE_MS));

    ESP_LOGI(TAG, "Deep-sleep button armed on GPIO%d",
             DEEP_SLEEP_BUTTON_GPIO);

    while (1) {
        if (gpio_get_level(DEEP_SLEEP_BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(DEEP_SLEEP_BUTTON_DEBOUNCE_MS));

            if (gpio_get_level(DEEP_SLEEP_BUTTON_GPIO) == 0) {
                request_deep_sleep("GPIO3 button");

                /* Only reached if entering deep sleep failed. */
                while (gpio_get_level(DEEP_SLEEP_BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(DEEP_SLEEP_BUTTON_POLL_MS));
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DEEP_SLEEP_BUTTON_POLL_MS));
    }
}

static void deep_sleep_timeout_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(DEEP_SLEEP_TIMEOUT_MS));

    ESP_LOGI(TAG, "No activity for %d ms; entering deep sleep",
             DEEP_SLEEP_TIMEOUT_MS);

    request_deep_sleep("30-second timeout");
    vTaskDelete(NULL);
}

static void camera_application_start_task(void *arg)
{
    (void)arg;

    esp_err_t ret;
    int video_cam_fd0 = -1;

    app_ui_set_status("Initializing display accelerator...");

    ppa_client_config_t ppa_srm_config = {
        .oper_type = PPA_OPERATION_SRM,
    };

    ret = ppa_register_client(&ppa_srm_config, &ppa_srm_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA client registration failed: %s", esp_err_to_name(ret));
        app_ui_show_error("PPA initialization failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    app_ui_set_status("Initializing face detection and recognition...");

    /*
     * Keep the existing service initialization unchanged, but defer it until
     * the user presses Start so the launcher GUI appears immediately at boot.
     */
    app_boot_initialize_services();

    diagnostics_start_ai_pipeline_monitor();

    ret = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cache alignment query failed: %s", esp_err_to_name(ret));
        app_ui_show_error("Memory initialization failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    lcd_fb_size = ALIGN_UP(
        (size_t)BSP_LCD_H_RES * BSP_LCD_V_RES *
            (APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? 2 : 3),
        data_cache_line_size);

    app_ui_set_status("Opening camera...");

    i2c_bus_ = bsp_i2c_get_handle();

    ret = app_video_main(i2c_bus_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Video initialization failed: %s", esp_err_to_name(ret));
        app_ui_show_error("Camera initialization failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    video_cam_fd0 = app_video_open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, APP_VIDEO_FMT);
    if (video_cam_fd0 < 0) {
        ESP_LOGE(TAG, "Video camera open failed");
        app_ui_show_error("Camera could not be opened. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    app_ui_set_status("Allocating camera buffers...");

    for (int i = 0; i < DISPLAY_BUF_NUM; i++) {
        display_buffer[i] = heap_caps_aligned_calloc(
            data_cache_line_size,
            1,
            lcd_fb_size,
            MALLOC_CAP_SPIRAM);

        if (!display_buffer[i]) {
            ESP_LOGE(TAG, "Failed to allocate display buffer %d", i);
            app_ui_show_error("Display-buffer allocation failed. Restart the device.");
            vTaskDelete(NULL);
            return;
        }
    }

    ESP_LOGI(TAG, "Using independent buffers, display_buf=%d camera_buf=%d",
             DISPLAY_BUF_NUM, CAMERA_BUF_NUM);

    void *camera_buf[CAMERA_BUF_NUM];
    for (int i = 0; i < CAMERA_BUF_NUM; i++) {
        camera_buf[i] = heap_caps_aligned_calloc(
            data_cache_line_size,
            1,
            app_video_get_buf_size(),
            MALLOC_CAP_SPIRAM);

        if (!camera_buf[i]) {
            ESP_LOGE(TAG, "Failed to allocate camera buffer %d", i);
            app_ui_show_error("Camera-buffer allocation failed. Restart the device.");
            vTaskDelete(NULL);
            return;
        }
    }

    ret = app_video_set_bufs(
        video_cam_fd0,
        CAMERA_BUF_NUM,
        (const void **)camera_buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera buffer setup failed: %s", esp_err_to_name(ret));
        app_ui_show_error("Camera-buffer setup failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    ret = app_video_register_frame_operation_cb(camera_video_frame_operation);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Frame callback registration failed: %s", esp_err_to_name(ret));
        app_ui_show_error("Camera callback setup failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    app_ui_set_status("Starting live application...");

    /*
     * Start capture while dummy draw is still disabled. Any frame arriving in
     * this tiny transition window is safely ignored by the callback.
     */
    ESP_LOGI(TAG, "[CORE-PROOF] Requesting video stream task on CPU0");
    ret = app_video_stream_task_start(video_cam_fd0, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Video stream task failed: %s", esp_err_to_name(ret));
        app_ui_show_error("Camera stream failed to start. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    /* Hide the launcher before granting direct display ownership to camera. */
    bsp_display_backlight_off();
    display_backlight_enabled = false;

    /*
     * Remove every launcher object while LVGL still owns the display. Dummy
     * draw is enabled only after the UI has been destroyed safely.
     */
    app_ui_destroy();
    vTaskDelay(pdMS_TO_TICKS(50));

    ret = esp_lv_adapter_set_dummy_draw(disp, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not enable camera display mode: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    dummy_draw_enabled = true;
    display_disable_lvgl_overlays();

    /* The first complete camera frame turns the backlight on again. */
    xTaskCreate(
        deep_sleep_timeout_task,
        "deep_sleep_timeout",
        2048,
        NULL,
        5,
        NULL);

    ESP_LOGI(TAG, "Launcher completed; camera application is running");
    vTaskDelete(NULL);
}

static void launcher_start_requested(void *user_data)
{
    (void)user_data;

    ESP_LOGI(TAG, "Launcher start callback received");

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
        1);

    if (created != pdPASS) {
        application_start_requested = false;
        ESP_LOGE(TAG, "Failed to create camera startup task");
        app_ui_show_error("Could not start application task. Restart the device.");
    }
}


void app_main(void)
{
    report_wake_reason();
    core_trace(TAG, "APP_MAIN_START");

    esp_err_t cpu_power_ret = cpu_power_init();
    if (cpu_power_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "CPU power management initialization failed: %s",
            esp_err_to_name(cpu_power_ret));
    }

    diagnostics_start_cpu_stats_monitor();

    /*
     * Phase 1: normal LVGL rendering. The user sees a proper launcher and the
     * camera/AI pipeline is not initialized until Start Camera is pressed.
     */
    disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG,
                 "Display/touch initialization failed. Check the selected Waveshare "
                 "display in menuconfig and the touch cable.");
        return;
    }

    /*
     * bsp_display_start() starts the LVGL worker asynchronously. Normal LVGL
     * mode is already active at boot, so do not force another dummy-mode
     * transition here.
     */
    vTaskDelay(pdMS_TO_TICKS(150));

    display_disable_lvgl_overlays();

    /*
     * The BSP initializes GT911 and registers the LVGL input device inside
     * bsp_display_start(). Use the BSP-owned handle directly.
     */
    launcher_touch_indev = bsp_display_get_input_dev();
    if (launcher_touch_indev) {
        ESP_LOGI(TAG,
                 "BSP touchscreen ready: type=%d",
                 (int)lv_indev_get_type(launcher_touch_indev));
    } else {
        ESP_LOGE(TAG, "BSP did not return an LVGL touchscreen input device");
    }

    app_ui_create(launcher_start_requested, NULL);

    if (!launcher_touch_indev) {
        app_ui_show_error(
            "Touch controller unavailable. Check the touch cable and BSP display selection.");
    }

    bsp_display_backlight_on();
    display_backlight_enabled = true;

    /* The physical button works both on the launcher and in camera mode. */
    BaseType_t button_task_created = xTaskCreatePinnedToCore(
        deep_sleep_button_task,
        "deep_sleep_button",
        2048,
        NULL,
        DEEP_SLEEP_BUTTON_TASK_PRIORITY,
        NULL,
        1);

    if (button_task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create deep-sleep button task");
        app_ui_show_error("Deep-sleep button task failed. Restart the device.");
    }
}

static void camera_video_frame_operation(
    uint8_t *camera_buf,
    uint8_t camera_buf_index,
    uint32_t camera_buf_hes,
    uint32_t camera_buf_ves,
    size_t camera_buf_len,
    void *user_data)
{

    if (!dummy_draw_enabled || dummy_mode_delay_flag)
    {
        return;
    }

    /*
     * Do not map camera_buf_index to an LCD frame buffer. The capture queue and
     * LCD scanout queue have independent timing and therefore independent phase.
     */
    (void)camera_buf_index;
    (void)camera_buf_len;
    (void)user_data;

    void *target_fb = display_buffer[display_buffer_index];
    if (!target_fb)
    {
        ESP_LOGE(TAG, "display buffer is NULL");
        return;
    }

    const uint32_t display_width = BSP_LCD_H_RES;
    const uint32_t display_height = BSP_LCD_V_RES;

    if (display_height == 0)
    {
        ESP_LOGE(TAG, "Display height is zero!");
        return;
    }

    uint32_t in_offset_x = 0;
    uint32_t in_offset_y = 0;

    uint32_t in_block_w = camera_buf_hes;
    uint32_t in_block_h = camera_buf_ves;

    float scale_x = (float)display_width / (float)camera_buf_hes;
    float scale_y = (float)display_height / (float)camera_buf_ves;

    ppa_srm_oper_config_t srm_config = {
        .in.buffer = camera_buf,
        .in.pic_w = camera_buf_hes,
        .in.pic_h = camera_buf_ves,
        .in.block_w = in_block_w,
        .in.block_h = in_block_h,
        .in.block_offset_x = in_offset_x,
        .in.block_offset_y = in_offset_y,
        .in.srm_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,

        .out.buffer = target_fb,
        .out.buffer_size = lcd_fb_size,
        .out.pic_w = display_width,
        .out.pic_h = display_height,
        .out.block_offset_x = 0,
        .out.block_offset_y = 0,
        .out.srm_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,

        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = scale_x,
        .scale_y = scale_y,
        .mirror_x = 0,
        .mirror_y = 0,
        .rgb_swap = 0,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "PPA SRM failed: %d", ret);
        return;
    }

    frame_count++;

    if ((frame_count % 100) == 0) {
        ESP_LOGI(TAG,
                "[CORE-PROOF] MAIN_SW frame=%" PRIu32
                " cpu=%d task=%s affinity=%d",
                frame_count,
                xPortGetCoreID(),
                pcTaskGetName(NULL),
                xTaskGetCoreID(xTaskGetCurrentTaskHandle()));
    }
    if ((frame_count % FACE_DETECT_INTERVAL) == 0) {
        face_box_t boxes[MAX_FACE_BOXES];
        diagnostics_ai_frame_sent_to_detector();

        /*
         * Recognition is far more expensive than detection and it runs inside
         * this video task, so every run stalls the display pipeline. Honour
         * FACE_RECOG_INTERVAL instead of recognising on every detection pass.
         */
        const bool run_recognition = ((frame_count % FACE_RECOG_INTERVAL) == 0);

    esp_err_t detect_power_ret = cpu_power_ai_begin();

    if (detect_power_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not request maximum CPU frequency for detection: %s",
            esp_err_to_name(detect_power_ret));
    }

    #if APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565
        int face_count = face_detect_run_rgb565(
            camera_buf,
            camera_buf_hes,
            camera_buf_ves,
            boxes,
            MAX_FACE_BOXES
        );
    #else
        int face_count = face_detect_run_rgb888(
            camera_buf,
            camera_buf_hes,
            camera_buf_ves,
            boxes,
            MAX_FACE_BOXES
        );
    #endif

    if (detect_power_ret == ESP_OK) {
        esp_err_t release_ret = cpu_power_ai_end();

        if (release_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Could not release detection CPU lock: %s",
                esp_err_to_name(release_ret));
        }
    }

        ESP_LOGI(TAG, "Detection ran, face_count=%d", face_count);
        diagnostics_ai_detection_result(face_count);

        if (face_count > 0) {
            int update_count = face_count;
            if (update_count > MAX_FACE_BOXES) {
                update_count = MAX_FACE_BOXES;
            }

            for (int i = 0; i < update_count; i++) {
                if (i < last_face_count) {
                    last_boxes[i].x1 = smooth_coord(last_boxes[i].x1, boxes[i].x1);
                    last_boxes[i].y1 = smooth_coord(last_boxes[i].y1, boxes[i].y1);
                    last_boxes[i].x2 = smooth_coord(last_boxes[i].x2, boxes[i].x2);
                    last_boxes[i].y2 = smooth_coord(last_boxes[i].y2, boxes[i].y2);
                    last_boxes[i].score = boxes[i].score;
                } else {
                    last_boxes[i] = boxes[i];
                }

                ESP_LOGI(TAG,
                        "Face %d score=%.2f box=[%d,%d,%d,%d]",
                        i,
                        boxes[i].score,
                        boxes[i].x1,
                        boxes[i].y1,
                        boxes[i].x2,
                        boxes[i].y2);

                if (i == 0 && run_recognition && boxes[i].score > 0.85f) {
                    char name[FACE_RECOG_MAX_NAME_LEN];
                    float recog_score = 0.0f;

                    esp_err_t recognition_power_ret = cpu_power_ai_begin();

                    if (recognition_power_ret != ESP_OK) {
                        ESP_LOGW(
                            TAG,
                            "Could not request maximum CPU frequency for recognition: %s",
                            esp_err_to_name(recognition_power_ret));
                    }

                    esp_err_t recog_ret = face_recognition_recognize(
                        camera_buf,
                        camera_buf_hes,
                        camera_buf_ves,
                        &boxes[i],
                        name,
                        sizeof(name),
                        &recog_score
                    );

                    if (recognition_power_ret == ESP_OK) {
                        esp_err_t release_ret = cpu_power_ai_end();

                        if (release_ret != ESP_OK) {
                            ESP_LOGW(
                                TAG,
                                "Could not release recognition CPU lock: %s",
                                esp_err_to_name(release_ret));
                        }
                    }

                    diagnostics_ai_recognition_result(recog_ret, name, recog_score);
                }
            }

            last_face_count = update_count;
            no_face_frames = 0;
        } else {
            no_face_frames++;

            if (no_face_frames >= FACE_BOX_HOLD_MISSES) {
                last_face_count = 0;
            }
        }
    }

    #if APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565
    if (last_face_count > 0) {
    #if SYNC_CACHE_AROUND_OVERLAY
        /* PPA updated PSRAM; invalidate stale CPU cache lines before drawing. */
        esp_cache_msync(target_fb, lcd_fb_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    #endif

        for (int i = 0; i < last_face_count; i++) {
            int lcd_x1 = last_boxes[i].x1 * display_width / camera_buf_hes;
            int lcd_y1 = last_boxes[i].y1 * display_height / camera_buf_ves;
            int lcd_x2 = last_boxes[i].x2 * display_width / camera_buf_hes;
            int lcd_y2 = last_boxes[i].y2 * display_height / camera_buf_ves;

            draw_rect_rgb565(
                (uint16_t *)target_fb,
                display_width,
                display_height,
                lcd_x1,
                lcd_y1,
                lcd_x2,
                lcd_y2,
                0xF800
            );
        }

    #if SYNC_CACHE_AROUND_OVERLAY
        /* CPU updated the rectangle; write it back before LCD DMA reads it. */
        esp_cache_msync(target_fb, lcd_fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    #endif
    }
    #endif

    uint32_t draw_w = display_width;
    uint32_t draw_h = display_height;

    if (dummy_draw_enabled && !dummy_mode_delay_flag)
    {
        ret = esp_lv_adapter_dummy_draw_blit(
            disp,
            0, 0,
            draw_w,
            draw_h,
            target_fb,
            true);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Dummy draw blit failed: %d", ret);
            return;
        }

        /* The submitted buffer is now owned by scanout; render into the other. */
        display_buffer_index = (display_buffer_index + 1) % DISPLAY_BUF_NUM;

        /* Never expose LVGL's startup/sysmon frame. Show only a complete camera frame. */
        if (!display_backlight_enabled)
        {
            bsp_display_backlight_on();
            display_backlight_enabled = true;
        }
    }
}
