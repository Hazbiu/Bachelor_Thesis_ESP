#include "app_ui.h"

#include <stdbool.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "settings/app_settings.h"

static const char *TAG = "app_ui";

static app_ui_start_callback_t s_start_callback;
static app_ui_settings_callback_t s_settings_callback;
static void *s_user_data;

static lv_obj_t *s_status_label;
static lv_obj_t *s_start_button;
static lv_obj_t *s_start_button_label;
static lv_obj_t *s_settings_button;
static lv_obj_t *s_settings_button_label;
static bool s_action_dispatched;

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

static void disable_launcher_actions(void)
{
    if (s_start_button) {
        lv_obj_add_state(s_start_button, LV_STATE_DISABLED);
    }
    if (s_settings_button) {
        lv_obj_add_state(s_settings_button, LV_STATE_DISABLED);
    }
}

static void dispatch_start_request(void)
{
    if (s_action_dispatched) {
        return;
    }

    const app_settings_snapshot_t settings = app_settings_get();
    if (!settings.camera_enabled) {
        if (s_status_label) {
            lv_label_set_text(
                s_status_label,
                "Camera is OFF. Enable it in Settings.");
            lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xD97706), 0);
        }
        return;
    }

    s_action_dispatched = true;
    ESP_LOGI(TAG, "Start Camera pressed; requesting camera application start");
    disable_launcher_actions();

    if (s_start_button_label) {
        lv_label_set_text(s_start_button_label, "Starting...");
    }
    if (s_status_label) {
        lv_label_set_text(s_status_label, "Preparing camera and face recognition...");
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x64748B), 0);
    }

    if (s_start_callback) {
        s_start_callback(s_user_data);
    }
}

static void dispatch_settings_request(void)
{
    if (s_action_dispatched) {
        return;
    }

    s_action_dispatched = true;
    ESP_LOGI(TAG, "Settings pressed; requesting fresh settings screen");
    disable_launcher_actions();

    if (s_settings_button_label) {
        lv_label_set_text(s_settings_button_label, "Opening Settings...");
    }

    if (s_settings_callback) {
        s_settings_callback(s_user_data);
    }
}

static void launcher_start_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        dispatch_start_request();
    }
}

static void launcher_settings_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        dispatch_settings_request();
    }
}

static void create_ready_pill(lv_obj_t *parent, bool dark_mode, bool camera_ready)
{
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_set_size(pill, 176, 46);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(pill, 23, 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_bg_color(
        pill,
        lv_color_hex(
            camera_ready
                ? (dark_mode ? 0x153A2A : 0xDCFCE7)
                : (dark_mode ? 0x422006 : 0xFEF3C7)),
        0);

    lv_obj_t *label = lv_label_create(pill);
    lv_label_set_text(label, camera_ready ? "SYSTEM READY" : "POWER SAVING");
    lv_obj_set_style_text_color(
        label,
        lv_color_hex(
            camera_ready
                ? (dark_mode ? 0x86EFAC : 0x15803D)
                : (dark_mode ? 0xFCD34D : 0xB45309)),
        0);
    lv_obj_center(label);
}

void app_ui_create(
    app_ui_start_callback_t start_callback,
    app_ui_settings_callback_t settings_callback,
    void *user_data)
{
    s_start_callback = start_callback;
    s_settings_callback = settings_callback;
    s_user_data = user_data;
    s_action_dispatched = false;

    const app_settings_snapshot_t settings = app_settings_get();
    const bool dark = settings.dark_mode;
    const bool camera_ready = settings.camera_enabled;

    esp_err_t lock_ret = app_ui_lock();
    if (lock_ret != ESP_OK) {
        return;
    }

    const uint32_t screen_bg = dark ? 0x08111F : 0xF3F7FA;
    const uint32_t panel_bg = dark ? 0x111C2E : 0xFFFFFF;
    const uint32_t primary_text = dark ? 0xF8FAFC : 0x172033;
    const uint32_t secondary_text = dark ? 0xA9B6C7 : 0x64748B;
    const uint32_t border = dark ? 0x29415F : 0xDFE7EE;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(screen_bg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_size(panel, LV_PCT(82), LV_PCT(76));
    lv_obj_center(panel);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(panel, 34, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(panel_bg), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_shadow_opa(panel, dark ? LV_OPA_20 : LV_OPA_10, 0);
    lv_obj_set_style_shadow_width(panel, 28, 0);
    lv_obj_set_style_shadow_ofs_y(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 44, 0);
    lv_obj_set_style_pad_row(panel, 20, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        panel,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);

    lv_obj_t *brand = lv_obj_create(panel);
    lv_obj_set_size(brand, 104, 104);
    lv_obj_remove_flag(brand, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(brand, 30, 0);
    lv_obj_set_style_border_width(brand, 0, 0);
    lv_obj_set_style_bg_color(brand, lv_color_hex(0x2F80ED), 0);

    lv_obj_t *brand_label = lv_label_create(brand);
    lv_label_set_text(brand_label, "AI");
    lv_obj_set_style_text_color(brand_label, lv_color_white(), 0);
    lv_obj_center(brand_label);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Face Recognition");
    lv_obj_set_style_text_color(title, lv_color_hex(primary_text), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *subtitle = lv_label_create(panel);
    lv_label_set_text(
        subtitle,
        "Secure local recognition with adaptive power management.");
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(subtitle, LV_PCT(80));
    lv_obj_set_style_text_color(subtitle, lv_color_hex(secondary_text), 0);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);

    create_ready_pill(panel, dark, camera_ready);

    lv_obj_t *status_box = lv_obj_create(panel);
    lv_obj_set_size(status_box, LV_PCT(74), 60);
    lv_obj_remove_flag(status_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(status_box, 18, 0);
    lv_obj_set_style_bg_color(
        status_box,
        lv_color_hex(dark ? 0x0B1525 : 0xF8FAFC),
        0);
    lv_obj_set_style_border_color(status_box, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(status_box, 1, 0);

    s_status_label = lv_label_create(status_box);
    lv_label_set_text(
        s_status_label,
        !settings.camera_enabled
            ? "Camera disabled in Settings"
            : (!settings.sdcard_enabled
                ? "Ready; microSD powers off after AI load"
                : "Touch ready"));
    lv_obj_set_style_text_color(
        s_status_label,
        lv_color_hex(
            camera_ready
                ? (dark ? 0x86EFAC : 0x15803D)
                : (dark ? 0xFCD34D : 0xB45309)),
        0);
    lv_obj_center(s_status_label);

    s_start_button = lv_button_create(panel);
    lv_obj_set_size(s_start_button, LV_PCT(74), 92);
    lv_obj_remove_flag(s_start_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_start_button, 22, 0);
    lv_obj_set_style_bg_color(s_start_button, lv_color_hex(0x2F80ED), 0);
    lv_obj_set_style_bg_color(s_start_button, lv_color_hex(0x1D5FAF), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_start_button, lv_color_hex(0x94A3B8), LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_start_button, launcher_start_event_cb, LV_EVENT_PRESSED, NULL);

    if (!camera_ready) {
        lv_obj_add_state(s_start_button, LV_STATE_DISABLED);
    }

    s_start_button_label = lv_label_create(s_start_button);
    lv_label_set_text(
        s_start_button_label,
        camera_ready
            ? "Start Camera"
            : "Camera Off");
    lv_obj_set_style_text_color(s_start_button_label, lv_color_white(), 0);
    lv_obj_center(s_start_button_label);

    s_settings_button = lv_button_create(panel);
    lv_obj_set_size(s_settings_button, LV_PCT(74), 80);
    lv_obj_remove_flag(s_settings_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_settings_button, 22, 0);
    lv_obj_set_style_bg_color(s_settings_button, lv_color_hex(panel_bg), 0);
    lv_obj_set_style_bg_color(
        s_settings_button,
        lv_color_hex(dark ? 0x1D2A3E : 0xEEF4F8),
        LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_settings_button, lv_color_hex(panel_bg), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(s_settings_button, lv_color_hex(0x2F80ED), 0);
    lv_obj_set_style_border_width(s_settings_button, 2, 0);
    lv_obj_add_event_cb(
        s_settings_button,
        launcher_settings_event_cb,
        LV_EVENT_PRESSED,
        NULL);

    s_settings_button_label = lv_label_create(s_settings_button);
    lv_label_set_text(s_settings_button_label, "Settings");
    lv_obj_set_style_text_color(s_settings_button_label, lv_color_hex(0x2F80ED), 0);
    lv_obj_center(s_settings_button_label);

    lv_obj_t *hint = lv_label_create(panel);
    lv_label_set_text(
        hint,
        "GPIO3 remains available for the configured sleep mode.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(82));
    lv_obj_set_style_text_color(hint, lv_color_hex(secondary_text), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

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
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x64748B), 0);
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
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xDC2626), 0);
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
    s_settings_button = NULL;
    s_settings_button_label = NULL;
    s_start_callback = NULL;
    s_settings_callback = NULL;
    s_user_data = NULL;
    s_action_dispatched = false;

    bsp_display_unlock();
}
