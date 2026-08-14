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
#include "app_sleep.h"

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

static const char *TAG = "app_main";

static ppa_client_handle_t ppa_srm_handle = NULL;
static size_t data_cache_line_size = 0;
static void *display_buffer[APP_DISPLAY_BUFFER_COUNT];
static size_t lcd_fb_size = 0;
static uint8_t display_buffer_index = 0;
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

    pin_screen_hide();
    vTaskDelay(pdMS_TO_TICKS(30));

    esp_err_t ret = esp_lv_adapter_set_dummy_draw(disp, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Could not restore camera display mode: %s",
                 esp_err_to_name(ret));
        xSemaphoreGive(display_mode_mutex);
        vTaskDelete(NULL);
        return;
    }

    last_face_count = 0;
    no_face_frames = 0;
    memset(last_face_names, 0, sizeof(last_face_names));
    memset(last_recognition_scores, 0, sizeof(last_recognition_scores));

    dummy_draw_enabled = true;

    portENTER_CRITICAL(&authentication_state_lock);
    pin_transition_pending = false;
    pin_screen_active = false;
    portEXIT_CRITICAL(&authentication_state_lock);

    app_sleep_notify_face_detected();
    xSemaphoreGive(display_mode_mutex);

    ESP_LOGI(TAG, "PIN accepted; camera display mode restored");
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
        1);

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

    esp_err_t ret = esp_lv_adapter_set_dummy_draw(disp, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Could not enable LVGL PIN-screen mode: %s",
                 esp_err_to_name(ret));
        authentication_mark_transition_failed();
        xSemaphoreGive(display_mode_mutex);
        vTaskDelete(NULL);
        return;
    }

    dummy_draw_enabled = false;
    vTaskDelay(pdMS_TO_TICKS(30));

    ret = pin_screen_show(pending_identity, pin_accepted_callback, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not create PIN screen: %s", esp_err_to_name(ret));

        esp_err_t restore_ret = esp_lv_adapter_set_dummy_draw(disp, true);
        if (restore_ret == ESP_OK) {
            dummy_draw_enabled = true;
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

    xSemaphoreGive(display_mode_mutex);
    ESP_LOGI(TAG, "Camera paused; PIN screen is active");
    vTaskDelete(NULL);
}

static void authentication_request_pin(const char *recognized_name)
{
    if (!recognized_name || recognized_name[0] == '\0' ||
        strcmp(recognized_name, "unknown") == 0 ||
        app_sleep_is_requested()) {
        return;
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
        return;
    }

    const size_t identity_length = strnlen(
        recognized_name,
        sizeof(pending_identity) - 1);
    memcpy(pending_identity, recognized_name, identity_length);
    pending_identity[identity_length] = '\0';

    BaseType_t created = xTaskCreatePinnedToCore(
        pin_screen_transition_task,
        "pin_screen",
        6144,
        NULL,
        7,
        NULL,
        1);

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PIN-screen transition task");
        authentication_mark_transition_failed();
    }
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
 * Called by cpu_power before it applies 180 MHz. The camera task remains
 * active, but no future callback is allowed to submit a display buffer.
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

    BaseType_t created = xTaskCreate(
        idle_scan_touch_poll_task,
        "idle_scan_touch",
        3072,
        NULL,
        4,
        NULL);

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

    ESP_LOGI(TAG, "ACTIVE 360 MHz confirmed; restoring MIPI-DSI and LVGL");

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

    disp = bsp_display_resume_from_light_sleep();
    if (disp == NULL) {
        ESP_LOGE(TAG, "ACTIVE display restoration failed");
        xSemaphoreGive(display_mode_mutex);
        esp_restart();
    }

    vTaskDelay(pdMS_TO_TICKS(150));
    launcher_touch_indev = bsp_display_get_input_dev();

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
    last_face_count = 0;
    no_face_frames = 0;

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
    ESP_LOGI(TAG, "ACTIVE camera display restored at 360 MHz");
    vTaskDelete(NULL);
}

/* Called only after cpu_power has restored the fixed 360 MHz policy. */
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

    BaseType_t created = xTaskCreate(
        resume_display_from_idle_scan_task,
        "idle_scan_resume",
        6144,
        NULL,
        7,
        NULL);

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

    disp = bsp_display_resume_from_light_sleep();
    if (disp == NULL) {
        ESP_LOGE(TAG, "Display reinitialization after Light-sleep failed");
        xSemaphoreGive(display_mode_mutex);
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(150));
    launcher_touch_indev = bsp_display_get_input_dev();

    esp_err_t ret = esp_lv_adapter_set_dummy_draw(disp, true);
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

    for (int i = 0; i < APP_DISPLAY_BUFFER_COUNT; i++) {
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
             APP_DISPLAY_BUFFER_COUNT, APP_CAMERA_BUFFER_COUNT);

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
        authentication_blocks_camera())
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

    void *target_fb = NULL;

    const uint32_t display_width = BSP_LCD_H_RES;
    const uint32_t display_height = BSP_LCD_V_RES;

    if (display_height == 0)
    {
        ESP_LOGE(TAG, "Display height is zero!");
        return;
    }

    esp_err_t ret = ESP_OK;

    if (!idle_scan_frame) {
        target_fb = display_buffer[display_buffer_index];
        if (!target_fb) {
            ESP_LOGE(TAG, "display buffer is NULL");
            return;
        }

        const uint32_t in_offset_x = 0;
        const uint32_t in_offset_y = 0;
        const uint32_t in_block_w = camera_buf_hes;
        const uint32_t in_block_h = camera_buf_ves;
        const float scale_x = (float)display_width / (float)camera_buf_hes;
        const float scale_y = (float)display_height / (float)camera_buf_ves;

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

        ret = ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PPA SRM failed: %d", ret);
            return;
        }
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
    /*
     * The interval grows as inactivity approaches Light-sleep. This reduces
     * CPU/AI duty cycle while face detection remains available throughout the
     * complete active window.
     */
    const uint32_t detect_interval_frames =
        cpu_power_get_face_detect_interval_frames();

    if ((frame_count % detect_interval_frames) == 0) {
        face_box_t boxes[APP_MAX_FACE_BOXES];
        diagnostics_ai_frame_sent_to_detector();

        /*
         * Recognition is far more expensive than detection and it runs inside
         * this video task, so every run stalls the display pipeline. Honour
         * APP_FACE_RECOG_INTERVAL_FRAMES instead of recognising on every detection pass.
         */
        const bool run_recognition = ((frame_count % APP_FACE_RECOG_INTERVAL_FRAMES) == 0);

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
            APP_MAX_FACE_BOXES
        );
    #else
        int face_count = face_detect_run_rgb888(
            camera_buf,
            camera_buf_hes,
            camera_buf_ves,
            boxes,
            APP_MAX_FACE_BOXES
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
        authentication_note_detection_result(face_count);

        if (idle_scan_frame) {
            if (face_count > 0) {
                ESP_LOGI(
                    TAG,
                    "IDLE-SCAN face detected; restoring 360 MHz before display");
                app_sleep_notify_face_detected();
            }

            /* Recognition and display work resume only after ACTIVE restore. */
            return;
        }

        if (face_count > 0) {
            /*
             * Every positive detection restarts the 30-second inactivity
             * window. Recognition success is deliberately not required:
             * known and unknown faces both keep the application awake.
             */
            app_sleep_notify_face_detected();

            int update_count = face_count;
            if (update_count > APP_MAX_FACE_BOXES) {
                update_count = APP_MAX_FACE_BOXES;
            }

            face_box_t updated_boxes[APP_MAX_FACE_BOXES] = {0};
            char updated_names[APP_MAX_FACE_BOXES][FACE_RECOG_MAX_NAME_LEN] = {{0}};
            float updated_recognition_scores[APP_MAX_FACE_BOXES] = {0};
            bool previous_box_used[APP_MAX_FACE_BOXES] = {false};

            for (int i = 0; i < update_count; i++) {
                int matched_previous = -1;
                float best_iou = 0.20f;

                for (int previous = 0; previous < last_face_count; previous++) {
                    if (previous_box_used[previous]) {
                        continue;
                    }

                    const float iou = face_box_iou(&boxes[i], &last_boxes[previous]);
                    if (iou > best_iou) {
                        best_iou = iou;
                        matched_previous = previous;
                    }
                }

                updated_boxes[i] = boxes[i];

                if (matched_previous >= 0) {
                    previous_box_used[matched_previous] = true;
                    updated_boxes[i].x1 = smooth_coord(last_boxes[matched_previous].x1, boxes[i].x1);
                    updated_boxes[i].y1 = smooth_coord(last_boxes[matched_previous].y1, boxes[i].y1);
                    updated_boxes[i].x2 = smooth_coord(last_boxes[matched_previous].x2, boxes[i].x2);
                    updated_boxes[i].y2 = smooth_coord(last_boxes[matched_previous].y2, boxes[i].y2);
                    snprintf(
                        updated_names[i],
                        sizeof(updated_names[i]),
                        "%s",
                        last_face_names[matched_previous]
                    );
                    updated_recognition_scores[i] =
                        last_recognition_scores[matched_previous];
                }

                ESP_LOGI(TAG,
                        "Face %d score=%.2f box=[%d,%d,%d,%d]",
                        i,
                        boxes[i].score,
                        boxes[i].x1,
                        boxes[i].y1,
                        boxes[i].x2,
                        boxes[i].y2);

                if (run_recognition && boxes[i].score > APP_FACE_RECOG_MIN_SCORE) {
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

                    if (recog_ret == ESP_OK) {
                        snprintf(
                            updated_names[i],
                            sizeof(updated_names[i]),
                            "%s",
                            name
                        );
                        updated_recognition_scores[i] = recog_score;

                        /*
                         * A known face is the first factor. Only a successful
                         * recognition may open the second-factor PIN screen.
                         */
                        authentication_request_pin(updated_names[i]);
                    } else {
                        snprintf(
                            updated_names[i],
                            sizeof(updated_names[i]),
                            "%s",
                            "unknown"
                        );
                        updated_recognition_scores[i] = 0.0f;
                    }

                    ESP_LOGI(
                        TAG,
                        "Face %d recognition: name=%s similarity=%.3f result=%s",
                        i,
                        updated_names[i],
                        updated_recognition_scores[i],
                        esp_err_to_name(recog_ret)
                    );
                }
            }

            memcpy(last_boxes, updated_boxes, sizeof(updated_boxes));
            memcpy(last_face_names, updated_names, sizeof(updated_names));
            memcpy(
                last_recognition_scores,
                updated_recognition_scores,
                sizeof(updated_recognition_scores)
            );

            last_face_count = update_count;
            no_face_frames = 0;
        } else {
            no_face_frames++;

            if (no_face_frames >= APP_FACE_BOX_HOLD_MISSES) {
                last_face_count = 0;
                memset(last_face_names, 0, sizeof(last_face_names));
                memset(last_recognition_scores, 0, sizeof(last_recognition_scores));
            }
        }
    }

    if (idle_scan_frame) {
        return;
    }

    #if APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565
    if (last_face_count > 0) {
    #if APP_SYNC_CACHE_AROUND_OVERLAY
        /* PPA updated PSRAM; invalidate stale CPU cache lines before drawing. */
        esp_cache_msync(target_fb, lcd_fb_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    #endif

        for (int i = 0; i < last_face_count; i++) {
            int lcd_x1 = last_boxes[i].x1 * display_width / camera_buf_hes;
            int lcd_y1 = last_boxes[i].y1 * display_height / camera_buf_ves;
            int lcd_x2 = last_boxes[i].x2 * display_width / camera_buf_hes;
            int lcd_y2 = last_boxes[i].y2 * display_height / camera_buf_ves;

            const bool recognized =
                last_face_names[i][0] != '\0' &&
                strcmp(last_face_names[i], "unknown") != 0;
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
                overlay_color
            );

            draw_face_label_rgb565(
                (uint16_t *)target_fb,
                display_width,
                display_height,
                lcd_x2,
                lcd_y1,
                last_face_names[i],
                overlay_color
            );
        }

    #if APP_SYNC_CACHE_AROUND_OVERLAY
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
        display_buffer_index = (display_buffer_index + 1) % APP_DISPLAY_BUFFER_COUNT;

        /* Never expose LVGL's startup/sysmon frame. Show only a complete camera frame. */
        if (!display_backlight_enabled)
        {
            bsp_display_backlight_on();
            display_backlight_enabled = true;
        }
    }
}
