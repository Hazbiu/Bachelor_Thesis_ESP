#include "app_ui.h"

#include <stdbool.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "app_ui";

static app_ui_start_callback_t s_start_callback;
static void *s_start_user_data;

static lv_obj_t *s_status_label;
static lv_obj_t *s_start_button;
static lv_obj_t *s_start_button_label;
static bool s_start_dispatched;

/*
 * Wait for the BSP LVGL mutex. The LVGL worker performs its first refresh
 * asynchronously after bsp_display_start(), so an immediate lock can fail.
 */
static esp_err_t app_ui_lock(void)
{
    esp_err_t ret = ESP_FAIL;

    for (int attempt = 1; attempt <= 20; attempt++) {
        ret = bsp_display_lock(500);
        if (ret == ESP_OK) {
            return ESP_OK;
        }

        ESP_LOGW(TAG,
                 "LVGL lock unavailable (%s), retry %d/20",
                 esp_err_to_name(ret),
                 attempt);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGE(TAG, "Could not acquire LVGL lock: %s", esp_err_to_name(ret));
    return ret;
}

static void dispatch_start_request(void)
{
    if (s_start_dispatched) {
        return;
    }

    s_start_dispatched = true;
    ESP_LOGI(TAG, "Launcher touch accepted; requesting camera application start");

    /* Prevent repeated starts while camera and AI services initialize. */
    if (s_start_button) {
        lv_obj_add_state(s_start_button, LV_STATE_DISABLED);
    }
    if (s_start_button_label) {
        lv_label_set_text(s_start_button_label, "Starting...");
    }
    if (s_status_label) {
        lv_label_set_text(s_status_label, "Preparing camera and face recognition...");
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xB8C2CC), 0);
    }

    if (s_start_callback) {
        s_start_callback(s_start_user_data);
    }
}

static void launcher_press_event_cb(lv_event_t *event)
{
    /*
     * Start on the initial press rather than LV_EVENT_CLICKED.
     *
     * CLICKED is sent only after release and only if LVGL decides that the
     * pointer did not scroll or drift outside the widget. A large GT911 panel
     * can report a few pixels of release movement, which can suppress CLICKED.
     * PRESSED is immediate and is the correct behavior for this launcher.
     */
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        dispatch_start_request();
    }
}

void app_ui_create(app_ui_start_callback_t start_callback, void *user_data)
{
    s_start_callback = start_callback;
    s_start_user_data = user_data;
    s_start_dispatched = false;

    esp_err_t lock_ret = app_ui_lock();
    if (lock_ret != ESP_OK) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x08111F), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /*
     * The launcher has only one action, so the entire background is also a
     * touch target. This makes startup robust while still keeping the visible
     * Start Camera button as the main control.
     */
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, launcher_press_event_cb, LV_EVENT_PRESSED, NULL);

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_size(panel, LV_PCT(72), LV_PCT(68));
    lv_obj_center(panel);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, launcher_press_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_set_style_radius(panel, 28, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x111C2E), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x29415F), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_pad_all(panel, 36, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        panel,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Face Recognition System");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF5F8FC), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *subtitle = lv_label_create(panel);
    lv_label_set_text(
        subtitle,
        "Tap Start Camera to initialize the camera and open live recognition.");
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(subtitle, LV_PCT(82));
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0xA9B6C7), 0);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(subtitle, 12, 0);
    lv_obj_set_style_pad_bottom(subtitle, 24, 0);

    lv_obj_t *status_box = lv_obj_create(panel);
    lv_obj_set_size(status_box, LV_PCT(72), 52);
    lv_obj_remove_flag(status_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(status_box, 18, 0);
    lv_obj_set_style_bg_color(status_box, lv_color_hex(0x0B1525), 0);
    lv_obj_set_style_bg_opa(status_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_box, 0, 0);
    lv_obj_set_style_pad_all(status_box, 0, 0);
    lv_obj_add_flag(status_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(status_box, launcher_press_event_cb, LV_EVENT_PRESSED, NULL);

    s_status_label = lv_label_create(status_box);
    lv_label_set_text(s_status_label, "Touch ready");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x75D59A), 0);
    lv_obj_center(s_status_label);

    s_start_button = lv_button_create(panel);
    lv_obj_set_size(s_start_button, 300, 86);
    lv_obj_remove_flag(s_start_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_start_button, 20, 0);
    lv_obj_set_style_bg_color(s_start_button, lv_color_hex(0x2F80ED), 0);
    lv_obj_set_style_bg_color(
        s_start_button,
        lv_color_hex(0x1D5FAF),
        LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(
        s_start_button,
        lv_color_hex(0x435268),
        LV_STATE_DISABLED);
    lv_obj_set_style_pad_top(s_start_button, 20, 0);
    lv_obj_set_style_pad_bottom(s_start_button, 20, 0);
    lv_obj_add_event_cb(s_start_button, launcher_press_event_cb, LV_EVENT_PRESSED, NULL);

    s_start_button_label = lv_label_create(s_start_button);
    lv_label_set_text(s_start_button_label, "Start Camera");
    lv_obj_set_style_text_color(s_start_button_label, lv_color_white(), 0);
    lv_obj_center(s_start_button_label);

    lv_obj_t *hint = lv_label_create(panel);
    lv_label_set_text(
        hint,
        "Press the button or tap anywhere on this launcher. GPIO3 remains available for deep sleep.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(86));
    lv_obj_set_style_text_color(hint, lv_color_hex(0x75849A), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(hint, 14, 0);

    bsp_display_unlock();
}

void app_ui_set_status(const char *text)
{
    if (!text || !s_status_label) {
        return;
    }

    if (app_ui_lock() != ESP_OK) {
        return;
    }

    if (s_status_label) {
        lv_label_set_text(s_status_label, text);
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xB8C2CC), 0);
    }

    bsp_display_unlock();
}

void app_ui_show_error(const char *text)
{
    if (!text || !s_status_label) {
        return;
    }

    if (app_ui_lock() != ESP_OK) {
        return;
    }

    if (s_status_label) {
        lv_label_set_text(s_status_label, text);
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xFF7A7A), 0);
    }
    if (s_start_button_label) {
        lv_label_set_text(s_start_button_label, "Restart device");
    }

    bsp_display_unlock();
}

void app_ui_destroy(void)
{
    if (app_ui_lock() != ESP_OK) {
        return;
    }

    lv_obj_clean(lv_screen_active());

    s_status_label = NULL;
    s_start_button = NULL;
    s_start_button_label = NULL;
    s_start_callback = NULL;
    s_start_user_data = NULL;
    s_start_dispatched = false;

    bsp_display_unlock();
}
