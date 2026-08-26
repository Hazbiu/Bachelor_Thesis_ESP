#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_video_init.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/ppa.h"
#include "platform/camera/video_capture.h"
#include "services/vision/face_detector.h"
#include "services/vision/face_recognizer.h"
#include "diagnostics/core_trace.h"
#include "diagnostics/app_logging.h"
#include "diagnostics/cpu_stats.h"
#include "diagnostics/ai_pipeline_status.h"
#include "app/app_boot.h"
#include "app_ui.h"
#include "pin_screen.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/esp32_p4_platform.h"
#include "lvgl.h"
#include "lv_demos.h"
#include "wake_up.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "power_save/cpu_power.h"
#include "config/app_config.h"
#include "config/app_features.h"
#include "config/log_config.h"
#include "app_sleep.h"
#include "power_save/component_wifi.h"

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

static void draw_filled_rect_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int x1,
    int y1,
    int x2,
    int y2,
    uint16_t color)
{
    if (!fb || fb_w == 0 || fb_h == 0) {
        return;
    }

    if (x1 > x2) {
        int temp = x1;
        x1 = x2;
        x2 = temp;
    }

    if (y1 > y2) {
        int temp = y1;
        y1 = y2;
        y2 = temp;
    }

    if (x2 < 0 || y2 < 0 || x1 >= (int)fb_w || y1 >= (int)fb_h) {
        return;
    }

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= (int)fb_w) x2 = (int)fb_w - 1;
    if (y2 >= (int)fb_h) y2 = (int)fb_h - 1;

    for (int y = y1; y <= y2; y++) {
        uint16_t *row = fb + y * fb_w;
        for (int x = x1; x <= x2; x++) {
            row[x] = color;
        }
    }
}

static void draw_thick_rect_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int x1,
    int y1,
    int x2,
    int y2,
    int thickness,
    uint16_t color)
{
    for (int inset = 0; inset < thickness; inset++) {
        draw_rect_rgb565(
            fb,
            fb_w,
            fb_h,
            x1 + inset,
            y1 + inset,
            x2 - inset,
            y2 - inset,
            color
        );
    }
}

/* Five-pixel-wide uppercase font. Lowercase folder names are shown uppercase. */
static const uint8_t s_font_5x7[36][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, /* 0 */
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* 1 */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, /* 2 */
    {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}, /* 3 */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, /* 4 */
    {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}, /* 5 */
    {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E}, /* 6 */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, /* 8 */
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E}, /* 9 */
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* A */
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, /* B */
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, /* C */
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, /* D */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, /* E */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, /* F */
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, /* G */
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* H */
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* I */
    {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E}, /* J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, /* K */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, /* L */
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, /* M */
    {0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11}, /* N */
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* O */
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, /* P */
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, /* Q */
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, /* R */
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, /* S */
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* U */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, /* W */
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, /* X */
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, /* Y */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, /* Z */
};

static const uint8_t *font_5x7_glyph(char character)
{
    if (character >= 'a' && character <= 'z') {
        character = (char)(character - 'a' + 'A');
    }

    if (character >= '0' && character <= '9') {
        return s_font_5x7[character - '0'];
    }

    if (character >= 'A' && character <= 'Z') {
        return s_font_5x7[10 + character - 'A'];
    }

    return NULL;
}

static void draw_large_text_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int start_x,
    int start_y,
    const char *text,
    int scale,
    uint16_t color)
{
    if (!fb || !text || scale <= 0) {
        return;
    }

    int cursor_x = start_x;

    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        const uint8_t *glyph = font_5x7_glyph(*cursor);

        if (glyph) {
            for (int row = 0; row < 7; row++) {
                for (int column = 0; column < 5; column++) {
                    if ((glyph[row] & (1U << (4 - column))) == 0) {
                        continue;
                    }

                    draw_filled_rect_rgb565(
                        fb,
                        fb_w,
                        fb_h,
                        cursor_x + column * scale,
                        start_y + row * scale,
                        cursor_x + (column + 1) * scale - 1,
                        start_y + (row + 1) * scale - 1,
                        color
                    );
                }
            }
        }

        cursor_x += 6 * scale;
    }
}

static void draw_face_label_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int box_x2,
    int box_y1,
    const char *name,
    uint16_t background_color)
{
    if (!name || name[0] == '\0') {
        return;
    }

    const int scale = APP_FACE_LABEL_FONT_SCALE;
    const int padding = 2 * scale;
    const int text_width = (int)strlen(name) * 6 * scale - scale;
    const int text_height = 7 * scale;
    const int label_width = text_width + 2 * padding;
    const int label_height = text_height + 2 * padding;

    int label_x2 = box_x2;
    if (label_x2 >= (int)fb_w) label_x2 = (int)fb_w - 1;
    if (label_x2 < label_width - 1) label_x2 = label_width - 1;

    int label_x1 = label_x2 - label_width + 1;
    int label_y2 = box_y1 - 1;
    int label_y1 = label_y2 - label_height + 1;

    if (label_y1 < 0) {
        label_y1 = box_y1;
        label_y2 = label_y1 + label_height - 1;
    }

    draw_filled_rect_rgb565(
        fb,
        fb_w,
        fb_h,
        label_x1,
        label_y1,
        label_x2,
        label_y2,
        background_color
    );

    draw_large_text_rgb565(
        fb,
        fb_w,
        fb_h,
        label_x1 + padding,
        label_y1 + padding,
        name,
        scale,
        0xFFFF
    );
}

static float face_box_iou(const face_box_t *a, const face_box_t *b)
{
    const int intersection_x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    const int intersection_y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    const int intersection_x2 = a->x2 < b->x2 ? a->x2 : b->x2;
    const int intersection_y2 = a->y2 < b->y2 ? a->y2 : b->y2;
    const int intersection_w = intersection_x2 > intersection_x1
        ? intersection_x2 - intersection_x1 : 0;
    const int intersection_h = intersection_y2 > intersection_y1
        ? intersection_y2 - intersection_y1 : 0;
    const int intersection_area = intersection_w * intersection_h;
    const int area_a = (a->x2 - a->x1) * (a->y2 - a->y1);
    const int area_b = (b->x2 - b->x1) * (b->y2 - b->y1);
    const int union_area = area_a + area_b - intersection_area;

    return union_area > 0 ? (float)intersection_area / (float)union_area : 0.0f;
}

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

static bool authentication_request_pin(const char *recognized_name);
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

static face_box_t last_boxes[APP_MAX_FACE_BOXES];
static char last_face_names[APP_MAX_FACE_BOXES][FACE_RECOG_MAX_NAME_LEN];
static float last_recognition_scores[APP_MAX_FACE_BOXES];
static int last_face_count = 0;
static int no_face_frames = 0;

/*
 * AI runs on CPU1 from a private, downscaled snapshot. The camera/display
 * task on CPU0 never waits for BlazeFace or MobileFaceNet. There is exactly
 * one snapshot slot: while AI is busy, newer camera frames are displayed but
 * intentionally dropped from the AI path. This prevents an old-frame queue.
 */
typedef struct {
    uint32_t frame_id;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t width;
    uint32_t height;
    size_t data_size;
    bool idle_scan_frame;
} ai_snapshot_job_t;

static uint8_t *ai_snapshot_buffer = NULL;
static size_t ai_snapshot_capacity = 0;
static TaskHandle_t ai_worker_task_handle = NULL;
/*
 * Serialize the complete detector -> recognizer inference chain.
 *
 * Both AI backends (ESP-DL and TFLM-FP32) enter through the same public
 * face_detect_* / face_recognition_* APIs below, so one mutex guarantees that
 * detector and recognizer can never overlap or be invoked out of order.
 */
static SemaphoreHandle_t ai_inference_mutex = NULL;
static portMUX_TYPE ai_worker_state_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE ai_result_lock = portMUX_INITIALIZER_UNLOCKED;
static bool ai_job_pending = false;
static bool ai_worker_busy = false;
static bool ai_worker_accepting = false;
static ai_snapshot_job_t ai_pending_job = {0};

i2c_master_bus_handle_t i2c_bus_;

static bool dummy_draw_enabled = false;
static bool application_start_requested = false;
static bool dummy_mode_delay_flag = false;

/*
 * Direct camera scanout and normal LVGL rendering must never own the display
 * at the same time. The frame callback and both authentication transitions
 * use this mutex to serialize the ownership hand-off.
 */
static SemaphoreHandle_t display_mode_mutex;

static portMUX_TYPE authentication_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool pin_transition_pending;
static bool pin_screen_active;
static bool pin_rearm_required;
static int pin_rearm_no_face_passes;
static char pending_identity[FACE_RECOG_MAX_NAME_LEN];

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


static void ai_results_clear(void)
{
    portENTER_CRITICAL(&ai_result_lock);
    last_face_count = 0;
    no_face_frames = 0;
    memset(last_boxes, 0, sizeof(last_boxes));
    memset(last_face_names, 0, sizeof(last_face_names));
    memset(last_recognition_scores, 0, sizeof(last_recognition_scores));
    portEXIT_CRITICAL(&ai_result_lock);
}

static esp_err_t face_boost_release(const char *reason)
{
    if (!cpu_power_is_face_boost_active()) {
        return ESP_OK;
    }

    esp_err_t ret = cpu_power_face_boost_end();
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not release face CPU boost (%s): %s",
            reason ? reason : "unspecified",
            esp_err_to_name(ret));
    }

    return ret;
}

static bool authentication_blocks_camera(void)
{
    bool blocked;

    portENTER_CRITICAL(&authentication_state_lock);
    blocked = pin_transition_pending || pin_screen_active;
    portEXIT_CRITICAL(&authentication_state_lock);

    return blocked;
}

static void authentication_note_detection_result(int face_count)
{
    bool rearmed = false;

    portENTER_CRITICAL(&authentication_state_lock);

    if (face_count > 0) {
        pin_rearm_no_face_passes = 0;
    } else if (pin_rearm_required &&
               !pin_transition_pending &&
               !pin_screen_active) {
        pin_rearm_no_face_passes++;

        if (pin_rearm_no_face_passes >= APP_FACE_BOX_HOLD_MISSES) {
            pin_rearm_required = false;
            pin_rearm_no_face_passes = 0;
            rearmed = true;
        }
    }

    portEXIT_CRITICAL(&authentication_state_lock);

    if (rearmed) {
        ESP_LOGI(TAG, "Authentication rearmed after the face left the camera");
    }
}

static void authentication_mark_transition_failed(void)
{
    portENTER_CRITICAL(&authentication_state_lock);
    pin_transition_pending = false;
    pin_screen_active = false;
    portEXIT_CRITICAL(&authentication_state_lock);

    face_boost_release("PIN transition failed");
}

static void camera_resume_task(void *arg)
{
    (void)arg;

    /* Leave the success state visible briefly before restoring live video. */
    vTaskDelay(pdMS_TO_TICKS(350));

    if (app_sleep_is_requested()) {
        vTaskDelete(NULL);
        return;
    }

    if (!display_mode_mutex ||
        xSemaphoreTake(display_mode_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
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
    pin_screen_hide();
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_err_t ret = esp_lv_adapter_set_dummy_draw(disp, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Could not restore camera display mode: %s",
                 esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        vTaskDelete(NULL);
        return;
    }

    ai_results_clear();
    display_buffer_index = 0;
    dummy_draw_enabled = true;

    ret = app_video_stream_task_restart(video_cam_fd0);
    if (ret != ESP_OK) {
        dummy_draw_enabled = false;
        ESP_LOGE(TAG,
                 "Could not restart camera after PIN screen: %s",
                 esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        vTaskDelete(NULL);
        return;
    }

    portENTER_CRITICAL(&authentication_state_lock);
    pin_transition_pending = false;
    pin_screen_active = false;
    portEXIT_CRITICAL(&authentication_state_lock);

    app_sleep_notify_face_detected();
    xSemaphoreGive(display_mode_mutex);

    ESP_LOGI(TAG, "PIN accepted; camera stream and display mode restored");
    vTaskDelete(NULL);
}

static void pin_accepted_callback(void *user_data)
{
    (void)user_data;

    if (app_sleep_is_requested()) {
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

    if (!display_mode_mutex ||
        xSemaphoreTake(display_mode_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out while waiting to show PIN screen");
        authentication_mark_transition_failed();
        vTaskDelete(NULL);
        return;
    }

    if (app_sleep_is_requested()) {
        authentication_mark_transition_failed();
        xSemaphoreGive(display_mode_mutex);
        vTaskDelete(NULL);
        return;
    }

    /*
     * Stop the actual V4L2 stream for the PIN screen instead of merely ignoring
     * frame callbacks. This removes camera/ISP work while LVGL handles touch
     * input and prevents an old camera framebuffer/face box from being scanned
     * out underneath the PIN UI.
     */
    esp_err_t ret = app_video_stream_task_stop(video_cam_fd0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Could not pause camera for PIN screen: %s",
                 esp_err_to_name(ret));
        authentication_mark_transition_failed();
        xSemaphoreGive(display_mode_mutex);
        vTaskDelete(NULL);
        return;
    }

    ai_results_clear();
    display_buffer_index = 0;

    ret = esp_lv_adapter_set_dummy_draw(disp, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Could not enable LVGL PIN-screen mode: %s",
                 esp_err_to_name(ret));
        (void)app_video_stream_task_restart(video_cam_fd0);
        authentication_mark_transition_failed();
        xSemaphoreGive(display_mode_mutex);
        vTaskDelete(NULL);
        return;
    }

    dummy_draw_enabled = false;

    ret = pin_screen_show(pending_identity, pin_accepted_callback, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not create PIN screen: %s", esp_err_to_name(ret));

        esp_err_t restore_ret = esp_lv_adapter_set_dummy_draw(disp, true);
        if (restore_ret == ESP_OK) {
            dummy_draw_enabled = true;
            (void)app_video_stream_task_restart(video_cam_fd0);
        } else {
            ESP_LOGE(TAG,
                     "Could not recover camera display mode: %s",
                     esp_err_to_name(restore_ret));
        }

        authentication_mark_transition_failed();
        xSemaphoreGive(display_mode_mutex);
        vTaskDelete(NULL);
        return;
    }

    portENTER_CRITICAL(&authentication_state_lock);
    pin_transition_pending = false;
    pin_screen_active = true;
    portEXIT_CRITICAL(&authentication_state_lock);

    /*
     * pin_screen_show() performs a synchronous full-screen LVGL refresh before
     * returning. Keep the 360 MHz AI boost through that refresh, then return to
     * the normal 180 MHz baseline exactly as before.
     */
    const esp_err_t boost_release_ret =
        face_boost_release("PIN screen fully rendered");

    xSemaphoreGive(display_mode_mutex);
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
static bool authentication_request_pin(const char *recognized_name)
{
    if (!recognized_name || recognized_name[0] == '\0' ||
        strcmp(recognized_name, "unknown") == 0 ||
        app_sleep_is_requested()) {
        return false;
    }

    bool reserved = false;

    portENTER_CRITICAL(&authentication_state_lock);

    if (!pin_transition_pending &&
        !pin_screen_active &&
        !pin_rearm_required) {
        pin_transition_pending = true;
        pin_rearm_required = true;
        pin_rearm_no_face_passes = 0;
        reserved = true;
    }

    portEXIT_CRITICAL(&authentication_state_lock);

    if (!reserved) {
        return false;
    }

    const size_t identity_length = strnlen(
        recognized_name,
        sizeof(pending_identity) - 1);
    memcpy(pending_identity, recognized_name, identity_length);
    pending_identity[identity_length] = '\0';

    return true;
}

static bool authentication_launch_pin_transition(void)
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

static size_t ai_snapshot_bytes_per_pixel(void)
{
#if APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565
    return 2U;
#else
    return 3U;
#endif
}

static void ai_snapshot_dimensions(
    uint32_t source_width,
    uint32_t source_height,
    uint32_t *snapshot_width,
    uint32_t *snapshot_height)
{
    if (snapshot_width == NULL || snapshot_height == NULL ||
        source_width == 0 || source_height == 0) {
        return;
    }

    if (source_width >= source_height) {
        *snapshot_width = APP_AI_SNAPSHOT_MAX_EDGE;
        *snapshot_height =
            (source_height * APP_AI_SNAPSHOT_MAX_EDGE + source_width / 2U) /
            source_width;
    } else {
        *snapshot_height = APP_AI_SNAPSHOT_MAX_EDGE;
        *snapshot_width =
            (source_width * APP_AI_SNAPSHOT_MAX_EDGE + source_height / 2U) /
            source_height;
    }

    if (*snapshot_width == 0) *snapshot_width = 1;
    if (*snapshot_height == 0) *snapshot_height = 1;
}

static bool copy_camera_to_ai_snapshot(
    const uint8_t *source,
    size_t source_len,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t snapshot_width,
    uint32_t snapshot_height,
    size_t *written_bytes)
{
    if (source == NULL || ai_snapshot_buffer == NULL || written_bytes == NULL ||
        source_width == 0 || source_height == 0 ||
        snapshot_width == 0 || snapshot_height == 0) {
        return false;
    }

    const size_t bytes_per_pixel = ai_snapshot_bytes_per_pixel();
    const size_t required_source =
        (size_t)source_width * source_height * bytes_per_pixel;
    const size_t required_snapshot =
        (size_t)snapshot_width * snapshot_height * bytes_per_pixel;

    if (source_len < required_source || required_snapshot > ai_snapshot_capacity) {
        return false;
    }

#if APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565
    const uint16_t *src = (const uint16_t *)source;
    uint16_t *dst = (uint16_t *)ai_snapshot_buffer;

    for (uint32_t y = 0; y < snapshot_height; y++) {
        const uint32_t src_y =
            (uint32_t)(((uint64_t)y * source_height) / snapshot_height);
        const uint16_t *src_row = src + (size_t)src_y * source_width;
        uint16_t *dst_row = dst + (size_t)y * snapshot_width;

        for (uint32_t x = 0; x < snapshot_width; x++) {
            const uint32_t src_x =
                (uint32_t)(((uint64_t)x * source_width) / snapshot_width);
            dst_row[x] = src_row[src_x];
        }
    }
#else
    for (uint32_t y = 0; y < snapshot_height; y++) {
        const uint32_t src_y =
            (uint32_t)(((uint64_t)y * source_height) / snapshot_height);

        for (uint32_t x = 0; x < snapshot_width; x++) {
            const uint32_t src_x =
                (uint32_t)(((uint64_t)x * source_width) / snapshot_width);
            const size_t src_offset =
                ((size_t)src_y * source_width + src_x) * 3U;
            const size_t dst_offset =
                ((size_t)y * snapshot_width + x) * 3U;
            ai_snapshot_buffer[dst_offset + 0] = source[src_offset + 0];
            ai_snapshot_buffer[dst_offset + 1] = source[src_offset + 1];
            ai_snapshot_buffer[dst_offset + 2] = source[src_offset + 2];
        }
    }
#endif

    *written_bytes = required_snapshot;
    return true;
}

static void scale_box_to_source(
    const face_box_t *snapshot_box,
    uint32_t snapshot_width,
    uint32_t snapshot_height,
    uint32_t source_width,
    uint32_t source_height,
    face_box_t *source_box)
{
    if (snapshot_box == NULL || source_box == NULL ||
        snapshot_width == 0 || snapshot_height == 0) {
        return;
    }

    *source_box = *snapshot_box;
    source_box->x1 = (int)((int64_t)snapshot_box->x1 * source_width / snapshot_width);
    source_box->x2 = (int)((int64_t)snapshot_box->x2 * source_width / snapshot_width);
    source_box->y1 = (int)((int64_t)snapshot_box->y1 * source_height / snapshot_height);
    source_box->y2 = (int)((int64_t)snapshot_box->y2 * source_height / snapshot_height);

    for (int i = 0; i + 1 < source_box->keypoint_count && i + 1 < 10; i += 2) {
        source_box->keypoints[i] =
            (int)((int64_t)snapshot_box->keypoints[i] * source_width / snapshot_width);
        source_box->keypoints[i + 1] =
            (int)((int64_t)snapshot_box->keypoints[i + 1] * source_height / snapshot_height);
    }
}

/*
 * Display-only expansion for the quantized BlazeFace box.
 *
 * The detector's keypoints and the snapshot-space box passed to MobileFaceNet
 * are deliberately NOT changed.  Only the source/display rectangle is widened.
 */
static void expand_box_for_display(
    face_box_t *box,
    uint32_t source_width,
    uint32_t source_height,
    float margin_ratio)
{
    if (box == NULL || source_width == 0 || source_height == 0 ||
        margin_ratio <= 0.0f) {
        return;
    }

    const int width = box->x2 - box->x1;
    const int height = box->y2 - box->y1;
    if (width <= 0 || height <= 0) {
        return;
    }

    const int margin_x = (int)((float)width * margin_ratio + 0.5f);
    const int margin_y = (int)((float)height * margin_ratio + 0.5f);

    box->x1 -= margin_x;
    box->y1 -= margin_y;
    box->x2 += margin_x;
    box->y2 += margin_y;

    if (box->x1 < 0) box->x1 = 0;
    if (box->y1 < 0) box->y1 = 0;
    if (box->x2 >= (int)source_width) box->x2 = (int)source_width - 1;
    if (box->y2 >= (int)source_height) box->y2 = (int)source_height - 1;
}

static void publish_detected_boxes(
    const face_box_t *boxes,
    int count,
    bool preserve_previous_names)
{
    if (count < 0) count = 0;
    if (count > APP_MAX_FACE_BOXES) count = APP_MAX_FACE_BOXES;

    face_box_t previous_boxes[APP_MAX_FACE_BOXES] = {0};
    char previous_names[APP_MAX_FACE_BOXES][FACE_RECOG_MAX_NAME_LEN] = {{0}};
    float previous_scores[APP_MAX_FACE_BOXES] = {0};
    int previous_count = 0;

    portENTER_CRITICAL(&ai_result_lock);
    previous_count = last_face_count;
    if (previous_count > APP_MAX_FACE_BOXES) previous_count = APP_MAX_FACE_BOXES;
    memcpy(previous_boxes, last_boxes, sizeof(previous_boxes));
    memcpy(previous_names, last_face_names, sizeof(previous_names));
    memcpy(previous_scores, last_recognition_scores, sizeof(previous_scores));
    portEXIT_CRITICAL(&ai_result_lock);

    face_box_t updated_boxes[APP_MAX_FACE_BOXES] = {0};
    char updated_names[APP_MAX_FACE_BOXES][FACE_RECOG_MAX_NAME_LEN] = {{0}};
    float updated_scores[APP_MAX_FACE_BOXES] = {0};
    bool previous_used[APP_MAX_FACE_BOXES] = {false};

    for (int i = 0; i < count; i++) {
        int matched_previous = -1;
        float best_iou = 0.20f;

        for (int previous = 0; previous < previous_count; previous++) {
            if (previous_used[previous]) continue;
            const float iou = face_box_iou(&boxes[i], &previous_boxes[previous]);
            if (iou > best_iou) {
                best_iou = iou;
                matched_previous = previous;
            }
        }

        updated_boxes[i] = boxes[i];

        if (matched_previous >= 0) {
            previous_used[matched_previous] = true;
            updated_boxes[i].x1 = smooth_coord(previous_boxes[matched_previous].x1, boxes[i].x1);
            updated_boxes[i].y1 = smooth_coord(previous_boxes[matched_previous].y1, boxes[i].y1);
            updated_boxes[i].x2 = smooth_coord(previous_boxes[matched_previous].x2, boxes[i].x2);
            updated_boxes[i].y2 = smooth_coord(previous_boxes[matched_previous].y2, boxes[i].y2);

            if (preserve_previous_names) {
                snprintf(updated_names[i], sizeof(updated_names[i]), "%s",
                         previous_names[matched_previous]);
                updated_scores[i] = previous_scores[matched_previous];
            }
        }
    }

    portENTER_CRITICAL(&ai_result_lock);
    memcpy(last_boxes, updated_boxes, sizeof(updated_boxes));
    memcpy(last_face_names, updated_names, sizeof(updated_names));
    memcpy(last_recognition_scores, updated_scores, sizeof(updated_scores));
    last_face_count = count;
    no_face_frames = 0;
    portEXIT_CRITICAL(&ai_result_lock);
}

static void publish_recognition_result(
    int index,
    const char *name,
    float score)
{
    if (index < 0 || index >= APP_MAX_FACE_BOXES || name == NULL) {
        return;
    }

    portENTER_CRITICAL(&ai_result_lock);
    if (index < last_face_count) {
        snprintf(last_face_names[index], sizeof(last_face_names[index]), "%s", name);
        last_recognition_scores[index] = score;
    }
    portEXIT_CRITICAL(&ai_result_lock);
}

static void publish_no_face_result(void)
{
    bool release_boost = false;

    portENTER_CRITICAL(&ai_result_lock);
    no_face_frames++;
    if (no_face_frames >= APP_FACE_BOX_HOLD_MISSES) {
        last_face_count = 0;
        memset(last_boxes, 0, sizeof(last_boxes));
        memset(last_face_names, 0, sizeof(last_face_names));
        memset(last_recognition_scores, 0, sizeof(last_recognition_scores));
        release_boost = true;
    }
    portEXIT_CRITICAL(&ai_result_lock);

    if (release_boost) {
        face_boost_release("face left camera");
    }
}

static void ai_worker_mark_idle(void)
{
    portENTER_CRITICAL(&ai_worker_state_lock);
    ai_worker_busy = false;
    portEXIT_CRITICAL(&ai_worker_state_lock);
}

/*
 * Preserve the existing >0.70 recognition gate for ESP-DL and TFLM-FP32.
 *
 * The INT8 detector's classifier output is quantized on a coarse grid. Its
 * candidate-generation threshold is exactly q=122 -> probability 0.50.
 * Accepted INT8 candidates must be allowed into MobileFaceNet so the second
 * stage can verify identity and request the PIN screen.
 */
static bool ai_detection_allows_recognition(float detector_score)
{
#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_INT8
    /*
     * The INT8 detector has already applied its own candidate threshold.
     * Do not filter the accepted candidate a second time before MobileFaceNet.
     * This avoids float/grid edge cases around the exact q=122 -> 0.50 score.
     */
    (void)detector_score;
    return true;
#else
    return detector_score > APP_FACE_RECOG_MIN_SCORE;
#endif
}

static void ai_worker_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "[CORE-PROOF] AI worker running: actual_cpu=%d task=%s",
             xPortGetCoreID(), pcTaskGetName(NULL));

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ai_snapshot_job_t job = {0};
        bool have_job = false;

        portENTER_CRITICAL(&ai_worker_state_lock);
        if (ai_job_pending) {
            job = ai_pending_job;
            ai_job_pending = false;
            ai_worker_busy = true;
            have_job = true;
        }
        portEXIT_CRITICAL(&ai_worker_state_lock);

        if (!have_job) {
            continue;
        }

        if (app_sleep_is_requested() || authentication_blocks_camera()) {
            ai_worker_mark_idle();
            continue;
        }

        /*
         * One mutex owns the complete detector -> recognizer chain. The shared
         * public APIs below dispatch to either ESP-DL or TFLM-FP32, therefore
         * this ordering and high-performance policy applies identically to both
         * backend selections.
         */
        if (ai_inference_mutex == NULL ||
            xSemaphoreTake(ai_inference_mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Could not acquire AI inference mutex");
            ai_worker_mark_idle();
            continue;
        }

        /*
         * Request 360 MHz BEFORE the detector starts. The previous code waited
         * for a positive detection before boosting, which meant the detector
         * itself ran at the 180 MHz baseline. Keep the same PM lock through the
         * recognizer when recognition follows.
         */
        esp_err_t boost_ret = cpu_power_face_boost_begin();
        if (boost_ret != ESP_OK) {
            ESP_LOGW(TAG, "Could not request AI high-performance CPU lock: %s",
                     esp_err_to_name(boost_ret));
        }

        if (data_cache_line_size > 0 && job.data_size > 0) {
            const size_t sync_size = ALIGN_UP(job.data_size, data_cache_line_size);
            (void)esp_cache_msync(
                ai_snapshot_buffer,
                sync_size,
                ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        }

        face_box_t snapshot_boxes[APP_MAX_FACE_BOXES] = {0};
        diagnostics_ai_frame_sent_to_detector();

#if APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565
        const int face_count = face_detect_run_rgb565(
            ai_snapshot_buffer,
            job.width,
            job.height,
            snapshot_boxes,
            APP_MAX_FACE_BOXES);
#else
        const int face_count = face_detect_run_rgb888(
            ai_snapshot_buffer,
            job.width,
            job.height,
            snapshot_boxes,
            APP_MAX_FACE_BOXES);
#endif

        ESP_LOGI(TAG,
                 "AI worker detection complete: frame=%" PRIu32 " faces=%d",
                 job.frame_id, face_count);
        diagnostics_ai_detection_result(face_count);
        authentication_note_detection_result(face_count);

        if (face_count <= 0) {
            publish_no_face_result();
            face_boost_release("detector completed with no face");
            xSemaphoreGive(ai_inference_mutex);
            ai_worker_mark_idle();
            continue;
        }

        /* A positive detector result is real activity even if recognition fails. */
        app_sleep_notify_face_detected();

#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_INT8
        printf(
            "[INT8-POSITIVE-PATH] frame=%" PRIu32
            " faces=%d idle_scan=%d display_suspended=%d\n",
            job.frame_id,
            face_count,
            job.idle_scan_frame ? 1 : 0,
            idle_scan_display_is_suspended() ? 1 : 0);
#endif

        int update_count = face_count;
        if (update_count > APP_MAX_FACE_BOXES) update_count = APP_MAX_FACE_BOXES;

        face_box_t source_boxes[APP_MAX_FACE_BOXES] = {0};
        for (int i = 0; i < update_count; i++) {
            scale_box_to_source(
                &snapshot_boxes[i],
                job.width,
                job.height,
                job.source_width,
                job.source_height,
                &source_boxes[i]);

#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_INT8
            expand_box_for_display(
                &source_boxes[i],
                job.source_width,
                job.source_height,
                APP_TFLM_INT8_DISPLAY_BOX_MARGIN_RATIO);
#endif

            ESP_LOGI(TAG,
                     "Face %d score=%.2f box=[%d,%d,%d,%d]",
                     i,
                     snapshot_boxes[i].score,
                     source_boxes[i].x1,
                     source_boxes[i].y1,
                     source_boxes[i].x2,
                     source_boxes[i].y2);
        }

        /* Publish red boxes immediately; recognition may take many seconds. */
        publish_detected_boxes(source_boxes, update_count, true);

        if (job.idle_scan_frame || idle_scan_display_is_suspended()) {
            ESP_LOGI(TAG,
                     "IDLE-SCAN face detected; recognition deferred until display restore");
            face_boost_release("IDLE-SCAN detector completed");
            xSemaphoreGive(ai_inference_mutex);
            ai_worker_mark_idle();
            continue;
        }

#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_INT8
        /*
         * Every accepted INT8 face should immediately enter MobileFaceNet.
         * The detector already runs asynchronously on CPU1 and the existing
         * mutex/360-MHz lock stays held through this recognition call.
         */
        const bool run_recognition = true;
#else
        const bool run_recognition =
            (job.frame_id % APP_FACE_RECOG_INTERVAL_FRAMES) == 0;
#endif
        bool pin_transition_requested = false;

        if (run_recognition) {
            /*
             * Do not release either the inference mutex or the 360 MHz PM lock
             * between detection and recognition. This guarantees the requested
             * DET -> REC serial execution at maximum CPU frequency.
             */
            for (int i = 0; i < update_count; i++) {
                if (!ai_detection_allows_recognition(
                        snapshot_boxes[i].score)) {
                    continue;
                }

                char name[FACE_RECOG_MAX_NAME_LEN] = "unknown";
                float recog_score = 0.0f;

#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_INT8
                printf(
                    "[INT8-RECOG-TRIGGER] frame=%" PRIu32
                    " face=%d detector_score=%.6f\n",
                    job.frame_id,
                    i,
                    (double)snapshot_boxes[i].score);
#endif

                esp_err_t recog_ret = face_recognition_recognize(
                    ai_snapshot_buffer,
                    job.width,
                    job.height,
                    &snapshot_boxes[i],
                    name,
                    sizeof(name),
                    &recog_score);

                diagnostics_ai_recognition_result(recog_ret, name, recog_score);

                if (recog_ret != ESP_OK) {
                    snprintf(name, sizeof(name), "%s", "unknown");
                    recog_score = 0.0f;
                }

                publish_recognition_result(i, name, recog_score);

                if (recog_ret == ESP_OK && authentication_request_pin(name)) {
                    pin_transition_requested = true;
                }

                ESP_LOGI(TAG,
                         "Face %d recognition: name=%s similarity=%.3f result=%s",
                         i, name, recog_score, esp_err_to_name(recog_ret));

                if (pin_transition_requested) {
                    break;
                }
            }
        }

        /*
         * Every AI model call is finished at this point. Release the inference
         * mutex before the PIN task is allowed to start. If authentication was
         * accepted, keep the 360 MHz PM lock only through the PIN screen's
         * synchronous first full render; pin_screen_transition_task() releases
         * it immediately afterwards.
         */
        if (!pin_transition_requested) {
            face_boost_release(
                run_recognition
                    ? "detector/recognizer chain completed without PIN transition"
                    : "detector completed without recognition");
        }

        xSemaphoreGive(ai_inference_mutex);
        ai_worker_mark_idle();

        if (pin_transition_requested) {
            (void)authentication_launch_pin_transition();
        }
    }
}

static bool ai_worker_pause_and_drain(uint32_t timeout_ms)
{
    const TickType_t start_tick = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    portENTER_CRITICAL(&ai_worker_state_lock);
    ai_worker_accepting = false;
    ai_job_pending = false;
    portEXIT_CRITICAL(&ai_worker_state_lock);

    for (;;) {
        bool busy;
        portENTER_CRITICAL(&ai_worker_state_lock);
        busy = ai_worker_busy;
        portEXIT_CRITICAL(&ai_worker_state_lock);

        if (!busy) {
            return true;
        }

        if ((xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void ai_worker_resume_accepting(void)
{
    portENTER_CRITICAL(&ai_worker_state_lock);
    ai_worker_accepting = true;
    portEXIT_CRITICAL(&ai_worker_state_lock);
}

static bool schedule_ai_snapshot(
    const uint8_t *camera_buf,
    size_t camera_buf_len,
    uint32_t camera_width,
    uint32_t camera_height,
    uint32_t current_frame,
    bool idle_scan_frame)
{
    if (ai_worker_task_handle == NULL || ai_snapshot_buffer == NULL ||
        camera_buf == NULL || camera_width == 0 || camera_height == 0) {
        return false;
    }

    bool reserved = false;
    portENTER_CRITICAL(&ai_worker_state_lock);
    if (ai_worker_accepting && !ai_worker_busy && !ai_job_pending) {
        ai_job_pending = true;
        reserved = true;
    }
    portEXIT_CRITICAL(&ai_worker_state_lock);

    if (!reserved) {
        return false;
    }

    uint32_t snapshot_width = 0;
    uint32_t snapshot_height = 0;
    ai_snapshot_dimensions(
        camera_width,
        camera_height,
        &snapshot_width,
        &snapshot_height);

    size_t written_bytes = 0;
    if (!copy_camera_to_ai_snapshot(
            camera_buf,
            camera_buf_len,
            camera_width,
            camera_height,
            snapshot_width,
            snapshot_height,
            &written_bytes)) {
        portENTER_CRITICAL(&ai_worker_state_lock);
        ai_job_pending = false;
        portEXIT_CRITICAL(&ai_worker_state_lock);
        ESP_LOGE(TAG, "Could not copy camera frame into AI snapshot");
        return false;
    }

    if (data_cache_line_size > 0) {
        const size_t sync_size = ALIGN_UP(written_bytes, data_cache_line_size);
        (void)esp_cache_msync(
            ai_snapshot_buffer,
            sync_size,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    portENTER_CRITICAL(&ai_worker_state_lock);
    ai_pending_job.frame_id = current_frame;
    ai_pending_job.source_width = camera_width;
    ai_pending_job.source_height = camera_height;
    ai_pending_job.width = snapshot_width;
    ai_pending_job.height = snapshot_height;
    ai_pending_job.data_size = written_bytes;
    ai_pending_job.idle_scan_frame = idle_scan_frame;
    portEXIT_CRITICAL(&ai_worker_state_lock);

    xTaskNotifyGive(ai_worker_task_handle);
    return true;
}

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

static void prepare_application_for_sleep(void *user_data)
{
    (void)user_data;

    /*
     * Stop the frame callback from starting more PPA, detection,
     * recognition or LCD operations during shutdown.
     */
    dummy_mode_delay_flag = true;

    if (!ai_worker_pause_and_drain(APP_AI_WORKER_DRAIN_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "AI worker did not drain before Deep-sleep shutdown");
    }

    face_boost_release("application sleep requested");

    /*
     * app_sleep.c switches the physical backlight off. Keep this
     * module's software state synchronized with the hardware state.
     */
    display_backlight_enabled = false;
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
    (void)bsp_touch_poll_for_light_sleep(&touched);

    while (idle_scan_display_is_suspended() &&
           !display_suspended_for_light_sleep &&
           !app_sleep_is_requested()) {
        vTaskDelay(pdMS_TO_TICKS(APP_LIGHT_SLEEP_TOUCH_POLL_MS));

        esp_err_t ret = bsp_touch_poll_for_light_sleep(&touched);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "IDLE-SCAN touch polling failed: %s",
                     esp_err_to_name(ret));
            break;
        }

        if (touched) {
            ESP_LOGI(TAG, "IDLE-SCAN touchscreen activity detected");
            app_sleep_notify_face_detected();
            break;
        }
    }

    vTaskDelete(NULL);
}

/*
 * Called by cpu_power before it applies the first 180 MHz idle baseline. The
 * camera task remains active, but no future callback is allowed to submit a
 * display buffer. The later 90 MHz stage reuses this suspended display.
 */
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
    bsp_display_backlight_off();
    display_backlight_enabled = false;

    esp_err_t ret = bsp_display_suspend_for_light_sleep();
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

    portENTER_CRITICAL(&authentication_state_lock);
    pin_transition_pending = false;
    pin_screen_active = false;
    pin_rearm_required = false;
    pin_rearm_no_face_passes = 0;
    pending_identity[0] = '\0';
    portEXIT_CRITICAL(&authentication_state_lock);

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
        app_sleep_is_requested()) {
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
    esp_err_t ret = app_video_stream_task_stop(video_cam_fd0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ACTIVE camera pause before display restore failed: %s",
                 esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        esp_restart();
    }

    log_psram_state("before_bsp_display_resume");
    disp = bsp_display_resume_from_light_sleep();
    if (disp == NULL) {
        ESP_LOGE(TAG, "ACTIVE display restoration failed");
        xSemaphoreGive(display_mode_mutex);
        esp_restart();
    }

    log_psram_state("after_bsp_display_resume");
    vTaskDelay(pdMS_TO_TICKS(150));
    launcher_touch_indev = bsp_display_get_input_dev();

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
    display_disable_lvgl_overlays();
    ai_results_clear();

    ret = app_video_stream_task_restart(video_cam_fd0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ACTIVE camera restart after display restore failed: %s",
                 esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        esp_restart();
    }

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
        cpu_power_is_face_boost_active() ? "on" : "off");
    vTaskDelete(NULL);
}

/* Called after cpu_power has restored the ACTIVE baseline/max DFS policy. */
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

    if (!ai_worker_pause_and_drain(APP_AI_WORKER_DRAIN_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Timed out draining AI worker before Light-sleep");
        return ESP_ERR_TIMEOUT;
    }

    /*
     * The worker may have found a face while this callback waited for a long
     * FP32 inference to finish. In that case app_sleep_notify_face_detected()
     * reset the inactivity timer, so cancel the claimed Light-sleep before any
     * display/camera hardware is destroyed.
     */
    if (!app_sleep_light_sleep_is_due()) {
        ai_worker_resume_accepting();
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

    esp_err_t ret = app_video_stream_task_stop(video_cam_fd0);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Camera stream did not stop for Light-sleep: %s",
            esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        return ret;
    }

    ret = app_video_prepare_sensor_for_light_sleep();
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
        ret = bsp_display_suspend_for_light_sleep();
        if (ret == ESP_OK) {
            trim_display_buffers_for_sleep();
        }
    }

    /* The adapter invalidates every LVGL display/input object on shutdown. */
    disp = NULL;
    launcher_touch_indev = NULL;
    dummy_draw_enabled = false;
    display_suspended_for_light_sleep = true;

    portENTER_CRITICAL(&authentication_state_lock);
    pin_transition_pending = false;
    pin_screen_active = false;
    pin_rearm_required = false;
    pin_rearm_no_face_passes = 0;
    pending_identity[0] = '\0';
    portEXIT_CRITICAL(&authentication_state_lock);

    xSemaphoreGive(display_mode_mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Display suspend completed with errors: %s",
            esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Camera and MIPI-DSI suspended for Light-sleep");
    return ESP_OK;
}

static esp_err_t resume_application_from_light_sleep(void *user_data)
{
    (void)user_data;

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

    ESP_LOGI(TAG, "Restoring MIPI-DSI and camera after Light-sleep activity");

    log_psram_state("before_bsp_display_resume");
    disp = bsp_display_resume_from_light_sleep();
    if (disp == NULL) {
        ESP_LOGE(TAG, "Display reinitialization after Light-sleep failed");
        xSemaphoreGive(display_mode_mutex);
        return ESP_FAIL;
    }

    log_psram_state("after_bsp_display_resume");
    vTaskDelay(pdMS_TO_TICKS(150));
    launcher_touch_indev = bsp_display_get_input_dev();

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
    display_disable_lvgl_overlays();

    ret = app_video_restore_sensor_after_light_sleep();
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "OV5647 sensor restore after Light-sleep failed: %s",
            esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        return ret;
    }

    ret = app_video_stream_task_restart(video_cam_fd0);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Camera stream restart after Light-sleep failed: %s",
            esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        return ret;
    }

    /* The first complete camera frame turns the backlight on again. */
    display_backlight_enabled = false;
    display_suspended_for_light_sleep = false;
    dummy_mode_delay_flag = false;
    ai_results_clear();
    ai_worker_resume_accepting();

    xSemaphoreGive(display_mode_mutex);
    ESP_LOGI(TAG, "Camera and display restored after Light-sleep");
    return ESP_OK;
}

static void camera_application_start_task(void *arg)
{
    (void)arg;

    esp_err_t ret;
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

#if APP_DIAGNOSTICS_AI_PIPELINE_ENABLED
    diagnostics_start_ai_pipeline_monitor();
#endif

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

    ret = allocate_display_buffers();
    if (ret != ESP_OK) {
        app_ui_show_error("Display-buffer allocation failed. Restart the device.");
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
        APP_CAMERA_BUFFER_COUNT,
        (const void **)camera_buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera buffer setup failed: %s", esp_err_to_name(ret));
        app_ui_show_error("Camera-buffer setup failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    const size_t ai_snapshot_raw_capacity =
        (size_t)APP_AI_SNAPSHOT_MAX_EDGE * APP_AI_SNAPSHOT_MAX_EDGE *
        ai_snapshot_bytes_per_pixel();
    ai_snapshot_capacity = ALIGN_UP(ai_snapshot_raw_capacity, data_cache_line_size);
    ai_snapshot_buffer = heap_caps_aligned_calloc(
        data_cache_line_size,
        1,
        ai_snapshot_capacity,
        MALLOC_CAP_SPIRAM);

    if (ai_snapshot_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %u-byte AI snapshot buffer",
                 (unsigned)ai_snapshot_capacity);
        app_ui_show_error("AI snapshot allocation failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    ai_inference_mutex = xSemaphoreCreateMutex();
    if (ai_inference_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create AI inference mutex");
        app_ui_show_error("AI synchronization failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    BaseType_t ai_created = xTaskCreatePinnedToCore(
        ai_worker_task,
        "ai_worker",
        APP_AI_WORKER_STACK_SIZE,
        NULL,
        APP_AI_WORKER_PRIORITY,
        &ai_worker_task_handle,
        APP_AI_WORKER_CORE);

    if (ai_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create asynchronous AI worker");
        app_ui_show_error("AI worker creation failed. Restart the device.");
        vTaskDelete(NULL);
        return;
    }

    ai_worker_resume_accepting();
    ESP_LOGI(TAG,
             "Async AI ready: core=%d snapshot_max=%ux%u bytes=%u",
             APP_AI_WORKER_CORE,
             (unsigned)APP_AI_SNAPSHOT_MAX_EDGE,
             (unsigned)APP_AI_SNAPSHOT_MAX_EDGE,
             (unsigned)ai_snapshot_capacity);

    display_mode_mutex = xSemaphoreCreateMutex();
    if (!display_mode_mutex) {
        ESP_LOGE(TAG, "Failed to create display-mode mutex");
        app_ui_show_error("Display synchronization failed. Restart the device.");
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
    ESP_LOGI(TAG,
             "[CORE-PROOF] Requesting video stream task on system CPU%d",
             APP_SYSTEM_WORKER_CORE);
    ret = app_video_stream_task_start(
        video_cam_fd0, APP_SYSTEM_WORKER_CORE, NULL);
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

    /* Start the 30-second face-inactivity window with the camera application. */
    ret = app_sleep_start_timeout();
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to start face-inactivity monitor: %s",
            esp_err_to_name(ret));
    }

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
        APP_SYSTEM_WORKER_CORE);

    if (created != pdPASS) {
        application_start_requested = false;
        ESP_LOGE(TAG, "Failed to create camera startup task");
        app_ui_show_error("Could not start application task. Restart the device.");
    }
}


void app_main(void)
{
    app_logging_init();
    /* ESP32-C6 is unused: keep CHIP_PU LOW permanently. */
    esp_err_t c6_ret = component_wifi_disable_for_deep_sleep();
    if (c6_ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to disable ESP32-C6: %s",
                 esp_err_to_name(c6_ret));
    }

    ESP_LOGI(TAG,
             "[CORE-PROOF] Application split: system_cpu=%d ai_cpu=%d",
             APP_SYSTEM_WORKER_CORE,
             APP_AI_WORKER_CORE);

    report_wake_reason();
    core_trace(TAG, "APP_MAIN_START");

    esp_err_t cpu_power_ret = cpu_power_init();
    if (cpu_power_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "CPU power management initialization failed: %s",
            esp_err_to_name(cpu_power_ret));
    }

#if APP_DIAGNOSTICS_CPU_STATS_ENABLED
    diagnostics_start_cpu_stats_monitor();
#endif

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

    esp_err_t light_callbacks_ret =
        app_sleep_register_light_sleep_callbacks(
            suspend_application_for_light_sleep,
            resume_application_from_light_sleep,
            NULL);

    if (light_callbacks_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to register Light-sleep transitions: %s",
            esp_err_to_name(light_callbacks_ret));

        app_ui_show_error(
            "Light-sleep setup failed. Restart the device.");
    }

    esp_err_t idle_callbacks_ret =
        cpu_power_register_idle_scan_callbacks(
            suspend_display_for_idle_scan,
            request_display_resume_from_idle_scan,
            NULL);

    if (idle_callbacks_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to register adaptive IDLE-SCAN transitions: %s",
            esp_err_to_name(idle_callbacks_ret));
        app_ui_show_error(
            "Adaptive CPU power setup failed. Restart the device.");
    }

    /* The physical button works both on the launcher and in camera mode. */
    esp_err_t sleep_button_ret = app_sleep_start_button_monitor(
        prepare_application_for_sleep,
        NULL);

    if (sleep_button_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to start deep-sleep button monitor: %s",
            esp_err_to_name(sleep_button_ret));

        app_ui_show_error(
            "Deep-sleep button task failed. Restart the device.");
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
        authentication_blocks_camera()) {
        return;
    }

    (void)camera_buf_index;
    (void)user_data;

    const uint32_t display_width = BSP_LCD_H_RES;
    const uint32_t display_height = BSP_LCD_V_RES;
    if (display_width == 0 || display_height == 0) {
        ESP_LOGE(TAG, "Display dimensions are invalid");
        return;
    }

    void *target_fb = NULL;
    esp_err_t ret = ESP_OK;

    /*
     * DISPLAY FIRST.
     *
     * The old implementation ran BlazeFace/MobileFaceNet before this block,
     * freezing the LCD for every inference. The video task now performs only
     * the PPA scale, overlays the most recently completed AI result, submits
     * the frame, and returns. AI runs independently on CPU1.
     */
    if (!idle_scan_frame) {
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
            .in.srm_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565
                ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,

            .out.buffer = target_fb,
            .out.buffer_size = lcd_fb_size,
            .out.pic_w = display_width,
            .out.pic_h = display_height,
            .out.block_offset_x = 0,
            .out.block_offset_y = 0,
            .out.srm_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565
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

#if APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565
        face_box_t overlay_boxes[APP_MAX_FACE_BOXES] = {0};
        char overlay_names[APP_MAX_FACE_BOXES][FACE_RECOG_MAX_NAME_LEN] = {{0}};
        int overlay_count = 0;

        portENTER_CRITICAL(&ai_result_lock);
        overlay_count = last_face_count;
        if (overlay_count > APP_MAX_FACE_BOXES) overlay_count = APP_MAX_FACE_BOXES;
        memcpy(overlay_boxes, last_boxes, sizeof(overlay_boxes));
        memcpy(overlay_names, last_face_names, sizeof(overlay_names));
        portEXIT_CRITICAL(&ai_result_lock);

        if (overlay_count > 0) {
#if APP_SYNC_CACHE_AROUND_OVERLAY
            esp_cache_msync(target_fb, lcd_fb_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
#endif
            for (int i = 0; i < overlay_count; i++) {
                const int lcd_x1 = overlay_boxes[i].x1 * display_width / camera_buf_hes;
                const int lcd_y1 = overlay_boxes[i].y1 * display_height / camera_buf_ves;
                const int lcd_x2 = overlay_boxes[i].x2 * display_width / camera_buf_hes;
                const int lcd_y2 = overlay_boxes[i].y2 * display_height / camera_buf_ves;

                const bool recognized =
                    overlay_names[i][0] != '\0' &&
                    strcmp(overlay_names[i], "unknown") != 0;
                const uint16_t overlay_color = recognized ? 0x07E0 : 0xF800;

                draw_thick_rect_rgb565(
                    (uint16_t *)target_fb,
                    display_width,
                    display_height,
                    lcd_x1,
                    lcd_y1,
                    lcd_x2,
                    lcd_y2,
                    APP_FACE_BOX_THICKNESS,
                    overlay_color);

                draw_face_label_rgb565(
                    (uint16_t *)target_fb,
                    display_width,
                    display_height,
                    lcd_x2,
                    lcd_y1,
                    overlay_names[i],
                    overlay_color);
            }
#if APP_SYNC_CACHE_AROUND_OVERLAY
            esp_cache_msync(target_fb, lcd_fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#endif
        }
#endif

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

            if (!display_backlight_enabled) {
                bsp_display_backlight_on();
                display_backlight_enabled = true;
            }
        }
    }

    frame_count++;

    if ((frame_count % 100U) == 0U) {
        ESP_LOGI(TAG,
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
        cpu_power_get_face_detect_interval_frames();

    if (detect_interval_frames > 0 &&
        (frame_count % detect_interval_frames) == 0U) {
        (void)schedule_ai_snapshot(
            camera_buf,
            camera_buf_len,
            camera_buf_hes,
            camera_buf_ves,
            frame_count,
            idle_scan_frame);
    }
}
