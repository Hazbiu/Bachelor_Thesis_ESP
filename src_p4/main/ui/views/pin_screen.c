#include "pin_screen.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app/configuration/app_configuration.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#define PIN_LENGTH 4

typedef struct {
    uint32_t screen_bg;
    uint32_t shell_bg;
    uint32_t surface_bg;
    uint32_t key_bg;
    uint32_t key_secondary_bg;
    uint32_t key_pressed_bg;
    uint32_t key_disabled_bg;
    uint32_t primary_text;
    uint32_t secondary_text;
    uint32_t muted_text;
    uint32_t border;
    uint32_t accent;
    uint32_t success_bg;
    uint32_t success_border;
    uint32_t success_text;
    uint32_t error_text;
    uint32_t dot_empty_bg;
    uint32_t dot_empty_border;
} pin_palette_t;

static const char *TAG = "pin_screen";
static const char *CORRECT_PIN = "1234";

static pin_screen_success_callback_t s_success_callback;
static void *s_success_user_data;

static lv_obj_t *s_pin_dots[PIN_LENGTH];
static lv_obj_t *s_status_label;
static lv_obj_t *s_keypad;

static char s_entered_pin[PIN_LENGTH + 1];
static size_t s_entered_length;
static bool s_visible;
static bool s_submission_finished;
static pin_palette_t s_palette;

static pin_palette_t palette_for(bool dark)
{
    const pin_palette_t palette = dark
        ? (pin_palette_t) {
            .screen_bg = 0x08111F,
            .shell_bg = 0x111827,
            .surface_bg = 0x172033,
            .key_bg = 0x1E2B3D,
            .key_secondary_bg = 0x182334,
            .key_pressed_bg = 0x244F9E,
            .key_disabled_bg = 0x273246,
            .primary_text = 0xF8FAFC,
            .secondary_text = 0xB7C2D0,
            .muted_text = 0x7F91A8,
            .border = 0x334155,
            .accent = 0x5B8CFF,
            .success_bg = 0x12382C,
            .success_border = 0x2B8A68,
            .success_text = 0x69E6A6,
            .error_text = 0xFF7A90,
            .dot_empty_bg = 0x26364D,
            .dot_empty_border = 0x4A5D77,
        }
        : (pin_palette_t) {
            .screen_bg = 0xF4F7FA,
            .shell_bg = 0xFFFFFF,
            .surface_bg = 0xF8FAFC,
            .key_bg = 0xEAF1F7,
            .key_secondary_bg = 0xF1F5F9,
            .key_pressed_bg = 0xD7E7F7,
            .key_disabled_bg = 0xE2E8F0,
            .primary_text = 0x172033,
            .secondary_text = 0x475569,
            .muted_text = 0x64748B,
            .border = 0xD6E1EA,
            .accent = 0x2477D4,
            .success_bg = 0xDCFCE7,
            .success_border = 0x86EFAC,
            .success_text = 0x15803D,
            .error_text = 0xDC2626,
            .dot_empty_bg = 0xE2E8F0,
            .dot_empty_border = 0x94A3B8,
        };

    return palette;
}

static esp_err_t pin_screen_lock(void)
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
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    ESP_LOGE(TAG, "Could not acquire LVGL lock: %s", esp_err_to_name(ret));
    return ret;
}

static void set_status(const char *text, uint32_t color)
{
    if (!s_status_label) {
        return;
    }

    lv_label_set_text(s_status_label, text);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(color), 0);
}

static void refresh_pin_dots(uint32_t active_color)
{
    for (size_t i = 0; i < PIN_LENGTH; i++) {
        if (!s_pin_dots[i]) {
            continue;
        }

        const bool filled = i < s_entered_length;

        lv_obj_set_style_bg_color(
            s_pin_dots[i],
            lv_color_hex(filled ? active_color : s_palette.dot_empty_bg),
            0);
        lv_obj_set_style_border_color(
            s_pin_dots[i],
            lv_color_hex(filled ? active_color : s_palette.dot_empty_border),
            0);
    }
}

static void reset_entry(void)
{
    memset(s_entered_pin, 0, sizeof(s_entered_pin));
    s_entered_length = 0;
    s_submission_finished = false;
    refresh_pin_dots(s_palette.accent);
}

static void submit_pin(void)
{
    if (s_entered_length != PIN_LENGTH || s_submission_finished) {
        return;
    }

    if (strcmp(s_entered_pin, CORRECT_PIN) != 0) {
        set_status("Incorrect PIN. Please try again.", s_palette.error_text);
        refresh_pin_dots(s_palette.error_text);
        memset(s_entered_pin, 0, sizeof(s_entered_pin));
        s_entered_length = 0;
        return;
    }

    s_submission_finished = true;
    set_status("Access granted. Returning to camera...", s_palette.success_text);
    refresh_pin_dots(s_palette.success_text);

    if (s_keypad) {
        lv_obj_add_state(s_keypad, LV_STATE_DISABLED);
    }

    ESP_LOGI(TAG, "Second authentication factor accepted");

    /*
     * This callback runs on the LVGL task. The application callback must only
     * schedule the camera-mode transition and return immediately.
     */
    if (s_success_callback) {
        s_success_callback(s_success_user_data);
    }
}

static void key_pressed_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESSED ||
        s_submission_finished) {
        return;
    }

    const char *key = (const char *)lv_event_get_user_data(event);
    if (!key) {
        return;
    }

    /*
     * The automatic Light/Deep inactivity policy is paused for the entire PIN
     * screen by Application Logic. PIN touches therefore do not need to keep
     * resetting the camera inactivity epoch.
     */
    if (strcmp(key, "Clear") == 0) {
        reset_entry();
        set_status("Enter your four-digit access PIN", s_palette.secondary_text);
        return;
    }

    if (strcmp(key, "Back") == 0) {
        if (s_entered_length > 0) {
            s_entered_length--;
            s_entered_pin[s_entered_length] = '\0';
        }

        set_status("Enter your four-digit access PIN", s_palette.secondary_text);
        refresh_pin_dots(s_palette.accent);
        return;
    }

    if (s_entered_length < PIN_LENGTH && key[0] >= '0' && key[0] <= '9') {
        if (s_entered_length == 0) {
            set_status("Enter your four-digit access PIN", s_palette.secondary_text);
        }

        s_entered_pin[s_entered_length++] = key[0];
        s_entered_pin[s_entered_length] = '\0';
        refresh_pin_dots(s_palette.accent);

        if (s_entered_length == PIN_LENGTH) {
            submit_pin();
        }
    }
}

static lv_obj_t *create_text_label(
    lv_obj_t *parent,
    const char *text,
    uint32_t color,
    lv_text_align_t alignment)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, alignment, 0);
    return label;
}

static lv_obj_t *create_key(
    lv_obj_t *parent,
    const char *text,
    bool secondary)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, LV_PCT(30), 60);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(
        button,
        lv_color_hex(secondary ? s_palette.key_secondary_bg : s_palette.key_bg),
        0);
    lv_obj_set_style_bg_color(
        button,
        lv_color_hex(s_palette.key_pressed_bg),
        LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(
        button,
        lv_color_hex(s_palette.key_disabled_bg),
        LV_STATE_DISABLED);
    lv_obj_set_style_border_color(button, lv_color_hex(s_palette.border), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_add_event_cb(button, key_pressed_event_cb, LV_EVENT_PRESSED, (void *)text);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(
        label,
        lv_color_hex(secondary ? s_palette.secondary_text : s_palette.primary_text),
        0);
    lv_obj_set_style_text_letter_space(label, 1, 0);
    lv_obj_center(label);

    return button;
}

esp_err_t pin_screen_show(
    const char *recognized_name,
    pin_screen_success_callback_t success_callback,
    void *user_data)
{
    const app_configuration_snapshot_t settings = app_configuration_get();
    s_palette = palette_for(settings.dark_mode);

    esp_err_t lock_ret = pin_screen_lock();
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }

    s_success_callback = success_callback;
    s_success_user_data = user_data;
    s_status_label = NULL;
    s_keypad = NULL;
    memset(s_pin_dots, 0, sizeof(s_pin_dots));
    reset_entry();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(s_palette.screen_bg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /*
     * Flat, opaque PIN UI: no gradients, large box shadows or animations.
     * This reduces blended-pixel work while the camera is already stopped.
     */
    lv_obj_t *shell = lv_obj_create(screen);
    lv_obj_set_size(shell, LV_PCT(92), LV_PCT(88));
    lv_obj_center(shell);
    lv_obj_remove_flag(shell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(shell, 22, 0);
    lv_obj_set_style_bg_color(shell, lv_color_hex(s_palette.shell_bg), 0);
    lv_obj_set_style_bg_opa(shell, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(shell, lv_color_hex(s_palette.border), 0);
    lv_obj_set_style_border_width(shell, 1, 0);
    lv_obj_set_style_pad_all(shell, 26, 0);
    lv_obj_set_style_pad_column(shell, 28, 0);
    lv_obj_set_flex_flow(shell, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        shell,
        LV_FLEX_ALIGN_SPACE_BETWEEN,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);

    lv_obj_t *information = lv_obj_create(shell);
    lv_obj_set_size(information, LV_PCT(44), LV_PCT(100));
    lv_obj_remove_flag(information, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(information, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(information, 0, 0);
    lv_obj_set_style_pad_all(information, 8, 0);
    lv_obj_set_style_pad_row(information, 14, 0);
    lv_obj_set_flex_flow(information, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        information,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_CENTER);

    lv_obj_t *verified_badge = lv_obj_create(information);
    lv_obj_set_size(verified_badge, 180, 42);
    lv_obj_remove_flag(verified_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(verified_badge, 18, 0);
    lv_obj_set_style_bg_color(verified_badge, lv_color_hex(s_palette.success_bg), 0);
    lv_obj_set_style_bg_opa(verified_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(verified_badge, lv_color_hex(s_palette.success_border), 0);
    lv_obj_set_style_border_width(verified_badge, 1, 0);
    lv_obj_set_style_pad_all(verified_badge, 0, 0);

    lv_obj_t *badge_label = lv_label_create(verified_badge);
    lv_label_set_text(badge_label, "FACE VERIFIED");
    lv_obj_set_style_text_color(badge_label, lv_color_hex(s_palette.success_text), 0);
    lv_obj_set_style_text_letter_space(badge_label, 1, 0);
    lv_obj_center(badge_label);

    lv_obj_t *step_title = create_text_label(
        information,
        "Second-step verification",
        s_palette.primary_text,
        LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(step_title, 1, 0);

    char identity_text[96];
    snprintf(
        identity_text,
        sizeof(identity_text),
        "Welcome, %s. Confirm your identity with your private access PIN.",
        (recognized_name && recognized_name[0] != '\0')
            ? recognized_name
            : "recognized user");

    create_text_label(
        information,
        identity_text,
        s_palette.secondary_text,
        LV_TEXT_ALIGN_LEFT);

    lv_obj_t *dot_row = lv_obj_create(information);
    lv_obj_set_size(dot_row, LV_PCT(100), 64);
    lv_obj_remove_flag(dot_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(dot_row, lv_color_hex(s_palette.surface_bg), 0);
    lv_obj_set_style_bg_opa(dot_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dot_row, lv_color_hex(s_palette.border), 0);
    lv_obj_set_style_border_width(dot_row, 1, 0);
    lv_obj_set_style_radius(dot_row, 16, 0);
    lv_obj_set_style_pad_all(dot_row, 0, 0);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        dot_row,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);

    for (size_t i = 0; i < PIN_LENGTH; i++) {
        s_pin_dots[i] = lv_obj_create(dot_row);
        lv_obj_set_size(s_pin_dots[i], 22, 22);
        lv_obj_remove_flag(s_pin_dots[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(s_pin_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_pin_dots[i], lv_color_hex(s_palette.dot_empty_bg), 0);
        lv_obj_set_style_bg_opa(s_pin_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_pin_dots[i], lv_color_hex(s_palette.dot_empty_border), 0);
        lv_obj_set_style_border_width(s_pin_dots[i], 2, 0);
        lv_obj_set_style_margin_left(s_pin_dots[i], 7, 0);
        lv_obj_set_style_margin_right(s_pin_dots[i], 7, 0);
    }

    s_status_label = create_text_label(
        information,
        "Enter your four-digit access PIN",
        s_palette.secondary_text,
        LV_TEXT_ALIGN_LEFT);

    create_text_label(
        information,
        "The camera resumes automatically after successful verification.",
        s_palette.muted_text,
        LV_TEXT_ALIGN_LEFT);

    s_keypad = lv_obj_create(shell);
    lv_obj_set_size(s_keypad, LV_PCT(48), LV_PCT(100));
    lv_obj_remove_flag(s_keypad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_keypad, lv_color_hex(s_palette.surface_bg), 0);
    lv_obj_set_style_bg_opa(s_keypad, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_keypad, lv_color_hex(s_palette.border), 0);
    lv_obj_set_style_border_width(s_keypad, 1, 0);
    lv_obj_set_style_radius(s_keypad, 20, 0);
    lv_obj_set_style_pad_all(s_keypad, 18, 0);
    lv_obj_set_style_pad_row(s_keypad, 10, 0);
    lv_obj_set_style_pad_column(s_keypad, 10, 0);
    lv_obj_set_flex_flow(s_keypad, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(
        s_keypad,
        LV_FLEX_ALIGN_SPACE_BETWEEN,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);

    static const char *keys[] = {
        "1", "2", "3",
        "4", "5", "6",
        "7", "8", "9",
        "Clear", "0", "Back",
    };

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const bool secondary = strcmp(keys[i], "Clear") == 0 ||
                               strcmp(keys[i], "Back") == 0;
        create_key(s_keypad, keys[i], secondary);
    }

    /*
     * Dummy-draw camera mode leaves the last camera image in the panel's
     * framebuffer. Force one complete LVGL render before returning so every
     * pixel is overwritten by the opaque PIN screen immediately.
     */
    lv_obj_update_layout(screen);
    lv_obj_invalidate(screen);
    lv_refr_now(lv_display_get_default());

    s_visible = true;
    bsp_display_unlock();

    ESP_LOGI(
        TAG,
        "PIN screen shown for recognized identity; theme=%s",
        settings.dark_mode ? "dark" : "light");
    return ESP_OK;
}

void pin_screen_hide(void)
{
    if (!s_visible) {
        return;
    }

    if (pin_screen_lock() != ESP_OK) {
        return;
    }

    lv_obj_clean(lv_screen_active());

    s_success_callback = NULL;
    s_success_user_data = NULL;
    s_status_label = NULL;
    s_keypad = NULL;
    memset(s_pin_dots, 0, sizeof(s_pin_dots));
    memset(s_entered_pin, 0, sizeof(s_entered_pin));
    s_entered_length = 0;
    s_submission_finished = false;
    s_visible = false;

    bsp_display_unlock();
}

bool pin_screen_is_visible(void)
{
    return s_visible;
}
