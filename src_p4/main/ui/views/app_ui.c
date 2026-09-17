#include "app_ui.h"

#include <stdbool.h>

#include "app/configuration/app_configuration.h"
#include "assets/launcher_heading.h"
#include "assets/university_logo.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

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

static void show_launcher_status(const char *text, uint32_t color)
{
    if (!s_status_label || !text) {
        return;
    }

    lv_label_set_text(s_status_label, text);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(color), 0);
    lv_obj_remove_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
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

    const app_configuration_snapshot_t settings = app_configuration_get();
    if (!settings.camera_enabled) {
        show_launcher_status("Camera is OFF. Enable it in Settings.", 0xD97706);
        return;
    }

    s_action_dispatched = true;
    ESP_LOGI(TAG, "Start Camera pressed; requesting camera application start");
    disable_launcher_actions();

    if (s_start_button_label) {
        lv_label_set_text(s_start_button_label, "Starting...");
    }
    show_launcher_status("Preparing camera and AI face recognition...", 0x64748B);

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

static void create_brand_logo(lv_obj_t *parent, bool dark_mode)
{
    if (university_logo_available) {
        /*
         * Native RGB565 artwork is used directly from flash. No JPEG decoder,
         * filesystem read, runtime scaling or animation is needed.
         */
        lv_obj_t *logo_card = lv_obj_create(parent);
        lv_obj_set_size(logo_card, LV_PCT(88), 190);
        lv_obj_remove_flag(logo_card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(logo_card, 18, 0);
        lv_obj_set_style_bg_color(logo_card, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(logo_card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(
            logo_card,
            lv_color_hex(dark_mode ? 0x334155 : 0xE2E8F0),
            0);
        lv_obj_set_style_border_width(logo_card, 1, 0);
        lv_obj_set_style_pad_all(logo_card, 10, 0);

        lv_obj_t *logo = lv_image_create(logo_card);
        lv_image_set_src(logo, &university_logo_image);
        lv_obj_center(logo);
        return;
    }

    /* Fallback if the university JPG has not yet been converted. */
    lv_obj_t *brand = lv_obj_create(parent);
    lv_obj_set_size(brand, LV_PCT(88), 150);
    lv_obj_remove_flag(brand, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(brand, 18, 0);
    lv_obj_set_style_border_width(brand, 1, 0);
    lv_obj_set_style_border_color(
        brand,
        lv_color_hex(dark_mode ? 0x334155 : 0xE2E8F0),
        0);
    lv_obj_set_style_bg_color(brand, lv_color_hex(dark_mode ? 0x172033 : 0xF8FAFC), 0);

    lv_obj_t *brand_label = lv_label_create(brand);
    lv_label_set_text(brand_label, "THU");
    lv_obj_set_style_text_color(
        brand_label,
        lv_color_hex(dark_mode ? 0xF8FAFC : 0x172033),
        0);
    lv_obj_center(brand_label);
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

    const app_configuration_snapshot_t settings = app_configuration_get();
    const bool dark = settings.dark_mode;
    const bool camera_ready = settings.camera_enabled;

    if (app_ui_lock() != ESP_OK) {
        return;
    }

    const uint32_t screen_bg = dark ? 0x0B1220 : 0xF4F7FA;
    const uint32_t panel_bg = dark ? 0x111827 : 0xFFFFFF;
    const uint32_t secondary_text = dark ? 0xA9B6C7 : 0x64748B;
    const uint32_t border = dark ? 0x334155 : 0xDCE5EC;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(screen_bg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * Flat opaque surfaces are intentional: fewer blended shadow/gradient
     * pixels means less LVGL rendering work on every invalidation.
     */
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_size(panel, LV_PCT(90), LV_PCT(80));
    lv_obj_center(panel);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(panel, 24, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(panel_bg), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 28, 0);
    lv_obj_set_style_pad_row(panel, 18, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        panel,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);

    create_brand_logo(panel, dark);

    /*
     * The heading is a pre-rendered native RGB565 asset: visibly larger and
     * bold without requiring additional runtime font scaling/render passes.
     */
    lv_obj_t *heading = lv_image_create(panel);
    lv_image_set_src(
        heading,
        dark ? &launcher_heading_dark_image : &launcher_heading_light_image);

    /*
     * No static SYSTEM READY / Touch ready indicators are drawn anymore.
     * Keep one hidden status label only for startup progress and real errors.
     */
    s_status_label = lv_label_create(panel);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status_label, LV_PCT(86));
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(secondary_text), 0);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);

    s_start_button = lv_button_create(panel);
    lv_obj_set_size(s_start_button, LV_PCT(78), 94);
    lv_obj_remove_flag(s_start_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_start_button, 18, 0);
    lv_obj_set_style_bg_color(s_start_button, lv_color_hex(0x2477D4), 0);
    lv_obj_set_style_bg_color(s_start_button, lv_color_hex(0x1D5FAF), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_start_button, lv_color_hex(0x94A3B8), LV_STATE_DISABLED);
    lv_obj_set_style_border_width(s_start_button, 0, 0);
    lv_obj_add_event_cb(s_start_button, launcher_start_event_cb, LV_EVENT_PRESSED, NULL);

    if (!camera_ready) {
        lv_obj_add_state(s_start_button, LV_STATE_DISABLED);
    }

    s_start_button_label = lv_label_create(s_start_button);
    lv_label_set_text(s_start_button_label, camera_ready ? "Start Camera" : "Camera Off");
    lv_obj_set_style_text_color(s_start_button_label, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(s_start_button_label, 1, 0);
    lv_obj_center(s_start_button_label);

    s_settings_button = lv_button_create(panel);
    lv_obj_set_size(s_settings_button, LV_PCT(78), 82);
    lv_obj_remove_flag(s_settings_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_settings_button, 18, 0);
    lv_obj_set_style_bg_color(s_settings_button, lv_color_hex(panel_bg), 0);
    lv_obj_set_style_bg_color(
        s_settings_button,
        lv_color_hex(dark ? 0x1E293B : 0xEEF4F8),
        LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_settings_button, lv_color_hex(0x2477D4), 0);
    lv_obj_set_style_border_width(s_settings_button, 2, 0);
    lv_obj_add_event_cb(
        s_settings_button,
        launcher_settings_event_cb,
        LV_EVENT_PRESSED,
        NULL);

    s_settings_button_label = lv_label_create(s_settings_button);
    lv_label_set_text(s_settings_button_label, "Settings");
    lv_obj_set_style_text_color(s_settings_button_label, lv_color_hex(0x2477D4), 0);
    lv_obj_set_style_text_letter_space(s_settings_button_label, 1, 0);
    lv_obj_center(s_settings_button_label);

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

    show_launcher_status(text, 0x64748B);
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

    show_launcher_status(text, 0xDC2626);

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
