#include <inttypes.h>
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


#define DEEP_SLEEP_TIMEOUT_MS 30000
#define ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))
static bool enrolled_once = false;
static bool enrollment_finished_this_boot = false;

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

static esp_lcd_panel_handle_t display_panel;
static ppa_client_handle_t ppa_srm_handle = NULL;
static size_t data_cache_line_size = 0;
static void *lcd_buffer[CONFIG_BSP_LCD_DPI_BUFFER_NUMS];
static lv_display_t *disp;
#define FACE_DETECT_INTERVAL 5
#define MAX_FACE_BOXES 5
#define FACE_RECOG_INTERVAL 20

static uint32_t frame_count = 0;

static face_box_t last_boxes[MAX_FACE_BOXES];
static int last_face_count = 0;
static int no_face_frames = 0;

#define FACE_BOX_HOLD_MISSES 3

i2c_master_bus_handle_t i2c_bus_;

static bool dummy_draw_enabled = true;
static bool dummy_mode_delay_flag = false;

static void calc_ppa_input_offset(uint32_t src_w, uint32_t src_h,
                                  uint32_t dst_w, uint32_t dst_h,
                                  uint32_t *offset_x, uint32_t *offset_y)
{
    *offset_x = (src_w > dst_w) ? (src_w - dst_w) / 2 : 0;
    *offset_y = (src_h > dst_h) ? (src_h - dst_h) / 2 : 0;

    if ((*offset_x + dst_w) > src_w)
    {
        *offset_x = 0;
    }
    if ((*offset_y + dst_h) > src_h)
    {
        *offset_y = 0;
    }
}

static void deep_sleep_timeout_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(DEEP_SLEEP_TIMEOUT_MS));

    ESP_LOGI(TAG, "No activity for %d ms; entering deep sleep",
             DEEP_SLEEP_TIMEOUT_MS);

    enter_deep_sleep();
}

void app_main(void)
{
    report_wake_reason();
    core_trace(TAG, "APP_MAIN_START");
    diagnostics_start_cpu_stats_monitor();
    diagnostics_start_ai_pipeline_monitor();
    disp = bsp_display_start();
    bsp_display_backlight_on();

    ESP_ERROR_CHECK(esp_lv_adapter_set_dummy_draw(disp, dummy_draw_enabled));

    display_panel = bsp_display_get_panel_handle();

    ppa_client_config_t ppa_srm_config = {
        .oper_type = PPA_OPERATION_SRM,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_srm_config, &ppa_srm_handle));

    app_boot_initialize_services();
    



    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size));

    i2c_bus_ = bsp_i2c_get_handle();

    esp_err_t ret = app_video_main(i2c_bus_);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "video main init failed with err=0x%x", ret);
        return;
    }

    int video_cam_fd0 = app_video_open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, APP_VIDEO_FMT);
    if (video_cam_fd0 < 0)
    {
        ESP_LOGE(TAG, "video cam open failed");
        return;
    }

    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(
        display_panel, 3,
        &lcd_buffer[0], &lcd_buffer[1], &lcd_buffer[2]));

    ESP_LOGI(TAG, "Using user defined buffer");
    void *camera_buf[2];
    for (int i = 0; i < 2; i++)
    {
        camera_buf[i] = heap_caps_aligned_calloc(
            data_cache_line_size,
            1,
            app_video_get_buf_size(),
            MALLOC_CAP_SPIRAM);
    }
    ESP_ERROR_CHECK(app_video_set_bufs(video_cam_fd0, 2, (void *)camera_buf));

    ESP_ERROR_CHECK(app_video_register_frame_operation_cb(camera_video_frame_operation));

    ESP_LOGI(TAG, "[CORE-PROOF] Requesting video stream task on CPU0");

    ESP_ERROR_CHECK(app_video_stream_task_start(video_cam_fd0, 0, NULL));

    xTaskCreate(
        deep_sleep_timeout_task,
        "deep_sleep_timeout",
        2048,
        NULL,
        5,
        NULL);

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

        .out.buffer = lcd_buffer[camera_buf_index],
        .out.buffer_size = ALIGN_UP(display_width * display_height *
                                        (APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? 2 : 3),
                                    data_cache_line_size),
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
                

                if (i == 0 && boxes[i].score > 0.85f) {
                    char name[FACE_RECOG_MAX_NAME_LEN];
                    float recog_score = 0.0f;

                    esp_err_t recog_ret = face_recognition_recognize(
                        camera_buf,
                        camera_buf_hes,
                        camera_buf_ves,
                        &boxes[i],
                        name,
                        sizeof(name),
                        &recog_score
                    );

                    
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
    for (int i = 0; i < last_face_count; i++) {
        int lcd_x1 = last_boxes[i].x1 * display_width / camera_buf_hes;
        int lcd_y1 = last_boxes[i].y1 * display_height / camera_buf_ves;
        int lcd_x2 = last_boxes[i].x2 * display_width / camera_buf_hes;
        int lcd_y2 = last_boxes[i].y2 * display_height / camera_buf_ves;

        draw_rect_rgb565(
            (uint16_t *)lcd_buffer[camera_buf_index],
            display_width,
            display_height,
            lcd_x1,
            lcd_y1,
            lcd_x2,
            lcd_y2,
            0xF800
        );
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
            lcd_buffer[camera_buf_index],
            true);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Dummy draw blit failed: %d", ret);
        }
    }
}