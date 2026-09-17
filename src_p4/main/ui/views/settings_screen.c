#include "settings_screen.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "app/configuration/app_configuration.h"

#if !LV_USE_ROLLER
#error "Sleep time picker requires LV_USE_ROLLER (CONFIG_LV_USE_ROLLER=y)"
#endif

typedef enum {
    SETTINGS_TOGGLE_THEME,
    SETTINGS_TOGGLE_ACTIVE_OPTIMIZATION,
    SETTINGS_TOGGLE_LIGHT_SLEEP,
    SETTINGS_TOGGLE_DEEP_SLEEP,
} settings_toggle_kind_t;

#define SETTINGS_TOGGLE_CAPACITY       4U
#define SETTINGS_TOGGLE_DEBOUNCE_US    250000LL

typedef struct {
    lv_obj_t *toggle;
    settings_toggle_kind_t kind;
    bool requested_enabled;
    bool is_sleep_delay;
    uint32_t requested_seconds;
} settings_toggle_request_t;

typedef struct {
    bool dark;
    uint32_t screen_bg;
    uint32_t header_bg;
    uint32_t card_bg;
    uint32_t row_bg;
    uint32_t primary_text;
    uint32_t secondary_text;
    uint32_t border;
    uint32_t accent;
} settings_palette_t;

static const char *TAG = "settings_screen";

static settings_screen_back_callback_t s_back_callback;
static void *s_user_data;
static lv_obj_t *s_status_label;
static lv_obj_t *s_back_button;
static lv_obj_t *s_toggles[SETTINGS_TOGGLE_CAPACITY];
static size_t s_toggle_count;
static bool s_navigation_dispatched;
static bool s_switch_update_in_progress;
static volatile bool s_settings_write_in_progress;
static int64_t s_last_toggle_event_us;
static settings_toggle_request_t s_pending_toggle;
static lv_obj_t *s_time_buttons[2];
static lv_obj_t *s_time_labels[2];
static lv_obj_t *s_sleep_details[2];
static lv_obj_t *s_time_overlay;
static lv_obj_t *s_minutes_roller;
static lv_obj_t *s_seconds_roller;
static lv_obj_t *s_time_save_button;
static lv_obj_t *s_time_cancel_button;
static lv_obj_t *s_time_status;
static settings_toggle_kind_t s_time_kind;

static void refresh_sleep_labels(const app_configuration_snapshot_t *settings);
static void close_time_picker(void);
static void set_time_picker_busy(bool busy);
static void open_time_picker_event_cb(lv_event_t *event);

static esp_err_t settings_screen_lock(void)
{
    esp_err_t ret = ESP_FAIL;

    for (int attempt = 1; attempt <= 20; attempt++) {
        ret = bsp_display_lock(500);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGE(TAG, "Could not acquire LVGL lock: %s", esp_err_to_name(ret));
    return ret;
}

static settings_palette_t palette_for(bool dark)
{
    settings_palette_t palette = {
        .dark = dark,
        .screen_bg = dark ? 0x08111F : 0xF3F7FA,
        .header_bg = dark ? 0x0C1728 : 0xE9F7F5,
        .card_bg = dark ? 0x111C2E : 0xFFFFFF,
        .row_bg = dark ? 0x162338 : 0xF8FAFC,
        .primary_text = dark ? 0xF8FAFC : 0x172033,
        .secondary_text = dark ? 0xA9B6C7 : 0x64748B,
        .border = dark ? 0x29415F : 0xDFE7EE,
        .accent = 0x16B8A6,
    };
    return palette;
}

static void set_status(const char *text, bool is_error)
{
    if (s_status_label == NULL || text == NULL) {
        return;
    }

    lv_label_set_text(s_status_label, text);
    lv_obj_set_style_text_color(
        s_status_label,
        lv_color_hex(is_error ? 0xDC2626 : 0x16A34A),
        0);
    lv_obj_remove_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *create_section_label(
    lv_obj_t *parent,
    const char *text,
    const settings_palette_t *palette)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, LV_PCT(90));
    lv_obj_set_style_text_color(label, lv_color_hex(palette->secondary_text), 0);
    lv_obj_set_style_pad_left(label, 8, 0);
    lv_obj_set_style_pad_top(label, 12, 0);
    return label;
}

static lv_obj_t *create_card(
    lv_obj_t *parent,
    const settings_palette_t *palette)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(90));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(palette->card_bg), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(palette->border), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        card,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);
    return card;
}

static void create_divider(lv_obj_t *parent, const settings_palette_t *palette)
{
    lv_obj_t *divider = lv_obj_create(parent);
    lv_obj_set_size(divider, LV_PCT(100), 1);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(palette->border), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);
}

static void create_user_row(
    lv_obj_t *card,
    const char *name,
    const settings_palette_t *palette)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_size(row, LV_PCT(100), 68);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(palette->row_bg), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(row, 14, 0);
    lv_obj_set_style_pad_right(row, 16, 0);

    lv_obj_t *avatar = lv_obj_create(row);
    lv_obj_set_size(avatar, 44, 44);
    lv_obj_align(avatar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_remove_flag(avatar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(avatar, 22, 0);
    lv_obj_set_style_border_width(avatar, 0, 0);
    lv_obj_set_style_bg_color(avatar, lv_color_hex(palette->accent), 0);

    char initial[2] = {
        name[0] != '\0' ? (char)toupper((unsigned char)name[0]) : '?',
        '\0',
    };
    lv_obj_t *initial_label = lv_label_create(avatar);
    lv_label_set_text(initial_label, initial);
    lv_obj_set_style_text_color(initial_label, lv_color_white(), 0);
    lv_obj_center(initial_label);

    lv_obj_t *name_label = lv_label_create(row);
    lv_label_set_text(name_label, name);
    lv_obj_set_style_text_color(name_label, lv_color_hex(palette->primary_text), 0);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 62, 0);
}

static void create_authorized_users(
    lv_obj_t *content,
    const settings_palette_t *palette,
    const app_configuration_authorized_users_t *users,
    esp_err_t users_ret)
{
    create_section_label(content, "AUTHORIZED USERS", palette);
    lv_obj_t *card = create_card(content, palette);

    lv_obj_t *caption = lv_label_create(card);
    lv_label_set_text(caption, "Enrollment folders on microSD");
    lv_obj_set_width(caption, LV_PCT(100));
    lv_obj_set_style_text_color(caption, lv_color_hex(palette->secondary_text), 0);
    lv_obj_set_style_pad_bottom(caption, 6, 0);

    if (users_ret != ESP_OK) {
        lv_obj_t *message = lv_label_create(card);
        lv_label_set_text(
            message,
            users_ret == ESP_ERR_NOT_FOUND
                ? "No /sdcard/enroll folder found."
                : "microSD could not be read. Check the card and reopen Settings.");
        lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(message, LV_PCT(100));
        lv_obj_set_style_text_color(message, lv_color_hex(0xDC2626), 0);
        return;
    }

    if (users->count == 0) {
        lv_obj_t *empty = lv_label_create(card);
        lv_label_set_text(empty, "No authorized users found.");
        lv_obj_set_style_text_color(empty, lv_color_hex(palette->secondary_text), 0);
        return;
    }

    for (size_t index = 0; index < users->count; index++) {
        create_user_row(card, users->names[index], palette);
    }
}

static bool setting_value_for_kind(
    const app_configuration_snapshot_t *settings,
    settings_toggle_kind_t kind)
{
    switch (kind) {
    case SETTINGS_TOGGLE_THEME:
        return settings->dark_mode;
    case SETTINGS_TOGGLE_ACTIVE_OPTIMIZATION:
        return settings->active_optimization_enabled;
    case SETTINGS_TOGGLE_LIGHT_SLEEP:
        return settings->light_sleep_enabled;
    case SETTINGS_TOGGLE_DEEP_SLEEP:
        return settings->deep_sleep_enabled;
    }

    return false;
}

static esp_err_t apply_toggle_setting(
    settings_toggle_kind_t kind,
    bool enabled)
{
    switch (kind) {
    case SETTINGS_TOGGLE_THEME:
        return app_configuration_set_dark_mode(enabled);
    case SETTINGS_TOGGLE_ACTIVE_OPTIMIZATION:
        return app_configuration_set_active_optimization_enabled(enabled);
    case SETTINGS_TOGGLE_LIGHT_SLEEP:
        return app_configuration_set_light_sleep_enabled(enabled);
    case SETTINGS_TOGGLE_DEEP_SLEEP:
        return app_configuration_set_deep_sleep_enabled(enabled);
    }

    return ESP_ERR_INVALID_ARG;
}

static const char *toggle_success_message(
    settings_toggle_kind_t kind,
    bool enabled)
{
    switch (kind) {
    case SETTINGS_TOGGLE_THEME:
        return enabled ? "Dark mode saved." : "Light mode saved.";
    case SETTINGS_TOGGLE_ACTIVE_OPTIMIZATION:
        return enabled
            ? "Active Mode Optimization enabled and saved."
            : "Full-power active mode enabled and saved.";
    case SETTINGS_TOGGLE_LIGHT_SLEEP:
        return enabled
            ? "Light-sleep mode enabled and saved."
            : "Light-sleep mode disabled and saved.";
    case SETTINGS_TOGGLE_DEEP_SLEEP:
        return enabled
            ? "Deep-sleep mode enabled and saved."
            : "Deep-sleep mode disabled and saved.";
    }

    return "Setting saved.";
}

static void sync_toggle_state(lv_obj_t *toggle, bool enabled)
{
    if (toggle == NULL) {
        return;
    }

    s_switch_update_in_progress = true;
    if (enabled) {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    }
    s_switch_update_in_progress = false;
}

static void set_settings_controls_enabled(bool enabled)
{
    for (size_t index = 0; index < s_toggle_count; index++) {
        if (enabled) {
            lv_obj_remove_state(s_toggles[index], LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_toggles[index], LV_STATE_DISABLED);
        }
    }

    for (size_t index = 0; index < 2; index++) {
        if (s_time_buttons[index] != NULL) {
            if (enabled) {
                lv_obj_remove_state(s_time_buttons[index], LV_STATE_DISABLED);
            } else {
                lv_obj_add_state(s_time_buttons[index], LV_STATE_DISABLED);
            }
        }
    }

    if (s_back_button != NULL) {
        if (enabled) {
            lv_obj_remove_state(s_back_button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_back_button, LV_STATE_DISABLED);
        }
    }
}

static void toggle_apply_task(void *arg)
{
    (void)arg;

    const settings_toggle_request_t request = s_pending_toggle;
    const esp_err_t ret = request.is_sleep_delay
        ? (request.kind == SETTINGS_TOGGLE_LIGHT_SLEEP
            ? app_configuration_set_light_sleep_delay_seconds(request.requested_seconds)
            : app_configuration_set_deep_sleep_delay_seconds(request.requested_seconds))
        : apply_toggle_setting(request.kind, request.requested_enabled);
    const app_configuration_snapshot_t settings = app_configuration_get();
    const bool saved_enabled = setting_value_for_kind(&settings, request.kind);
    const bool rebuild_theme =
        ret == ESP_OK && !request.is_sleep_delay && request.kind == SETTINGS_TOGGLE_THEME;

    if (settings_screen_lock() == ESP_OK) {
        refresh_sleep_labels(&settings);
        if (request.is_sleep_delay) {
            set_time_picker_busy(false);
            if (ret == ESP_OK) {
                close_time_picker();
                set_status("Sleep time saved.", false);
            } else if (s_time_status != NULL) {
                lv_label_set_text(s_time_status, "Could not save. Try again or cancel.");
                lv_obj_set_style_text_color(s_time_status, lv_color_hex(0xDC2626), 0);
            }
        } else {
            sync_toggle_state(request.toggle, saved_enabled);
            set_status(ret == ESP_OK
                ? toggle_success_message(request.kind, saved_enabled)
                : "Change failed and was not saved. Check the serial log.",
                ret != ESP_OK);
        }

        if (!rebuild_theme) {
            s_settings_write_in_progress = false;
            set_settings_controls_enabled(s_time_overlay == NULL);
        }
        bsp_display_unlock();
    } else {
        /* Do not re-enable navigation while a worker could still hold UI
         * pointers. The existing display-lock error remains in the serial log. */
        ESP_LOGE(TAG, "Settings saved=%s; UI lock unavailable", ret == ESP_OK ? "yes" : "no");
    }

    if (rebuild_theme) {
        vTaskDelay(pdMS_TO_TICKS(40));
        settings_screen_create(s_back_callback, s_user_data);
    }

    vTaskDelete(NULL);
}

static void toggle_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED ||
        s_switch_update_in_progress) {
        return;
    }

    lv_obj_t *toggle = lv_event_get_target_obj(event);
    const settings_toggle_kind_t kind =
        (settings_toggle_kind_t)(uintptr_t)lv_event_get_user_data(event);
    const int64_t now_us = esp_timer_get_time();

    if (s_navigation_dispatched || s_time_overlay != NULL ||
        s_settings_write_in_progress ||
        (s_last_toggle_event_us != 0 &&
         now_us - s_last_toggle_event_us < SETTINGS_TOGGLE_DEBOUNCE_US)) {
        const app_configuration_snapshot_t settings = app_configuration_get();
        sync_toggle_state(toggle, setting_value_for_kind(&settings, kind));
        return;
    }

    s_last_toggle_event_us = now_us;
    s_settings_write_in_progress = true;
    s_pending_toggle.is_sleep_delay = false;
    s_pending_toggle.toggle = toggle;
    s_pending_toggle.kind = kind;
    s_pending_toggle.requested_enabled =
        lv_obj_has_state(toggle, LV_STATE_CHECKED);

    set_settings_controls_enabled(false);
    set_status("Applying and saving setting...", false);

    BaseType_t created = xTaskCreate(
        toggle_apply_task,
        "settings_apply",
        6144,
        NULL,
        5,
        NULL);

    if (created != pdPASS) {
        const app_configuration_snapshot_t settings = app_configuration_get();
        sync_toggle_state(toggle, setting_value_for_kind(&settings, kind));
        set_settings_controls_enabled(true);
        s_settings_write_in_progress = false;
        set_status("Could not start the settings worker. Try again.", true);
    }
}

static void refresh_sleep_labels(const app_configuration_snapshot_t *settings)
{
    const uint32_t seconds[2] = {
        settings->light_sleep_delay_seconds, settings->deep_sleep_delay_seconds,
    };
    for (size_t index = 0; index < 2; index++) {
        if (s_time_labels[index] != NULL) {
            char text[16];
            snprintf(text, sizeof(text), "%02um %02us",
                     (unsigned)(seconds[index] / 60U), (unsigned)(seconds[index] % 60U));
            lv_label_set_text(s_time_labels[index], text);
        }
    }
    if (s_sleep_details[0] != NULL) {
        lv_label_set_text(s_sleep_details[0], "After Active inactivity");
    }
    if (s_sleep_details[1] != NULL) {
        lv_label_set_text(s_sleep_details[1], settings->light_sleep_enabled
            ? "After Light Sleep starts" : "After Active inactivity");
    }
}

static uint32_t picker_seconds(void)
{
    return (uint32_t)lv_roller_get_selected(s_minutes_roller) * 60U +
           (uint32_t)lv_roller_get_selected(s_seconds_roller);
}

static void set_time_picker_busy(bool busy)
{
    lv_obj_t *controls[] = {s_minutes_roller, s_seconds_roller,
                           s_time_save_button, s_time_cancel_button};
    for (size_t index = 0; index < sizeof(controls) / sizeof(controls[0]); index++) {
        if (controls[index] == NULL) {
            continue;
        }
        if (busy) {
            lv_obj_add_state(controls[index], LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(controls[index], LV_STATE_DISABLED);
        }
    }
    if (!busy && s_time_save_button != NULL &&
        picker_seconds() < APP_SLEEP_DELAY_MIN_SECONDS) {
        lv_obj_add_state(s_time_save_button, LV_STATE_DISABLED);
    }
}

static void close_time_picker(void)
{
    if (s_time_overlay == NULL) {
        return;
    }
    lv_obj_t *overlay = s_time_overlay;
    s_time_overlay = NULL;
    s_minutes_roller = NULL;
    s_seconds_roller = NULL;
    s_time_save_button = NULL;
    s_time_cancel_button = NULL;
    s_time_status = NULL;
    lv_obj_delete(overlay);
    set_settings_controls_enabled(!s_settings_write_in_progress);
}

static void cancel_time_picker_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_settings_write_in_progress) {
        return;
    }
    if (lv_event_get_target_obj(event) == s_time_overlay ||
        lv_event_get_target_obj(event) == s_time_cancel_button) {
        close_time_picker();
    }
}

static void time_roller_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED ||
        s_settings_write_in_progress || s_time_status == NULL) {
        return;
    }
    const bool valid = picker_seconds() >= APP_SLEEP_DELAY_MIN_SECONDS;
    const app_configuration_snapshot_t settings = app_configuration_get();
    const settings_palette_t palette = palette_for(settings.dark_mode);
    lv_label_set_text(s_time_status, valid
        ? "Swipe minutes and seconds, then set time." : "Choose at least 1 second.");
    lv_obj_set_style_text_color(s_time_status,
        lv_color_hex(valid ? palette.secondary_text : 0xDC2626), 0);
    set_time_picker_busy(false);
}

static void save_time_picker_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED ||
        s_settings_write_in_progress || s_time_overlay == NULL) {
        return;
    }
    const uint32_t seconds = picker_seconds();
    if (seconds < APP_SLEEP_DELAY_MIN_SECONDS || seconds > APP_SLEEP_DELAY_MAX_SECONDS) {
        lv_label_set_text(s_time_status, "Choose a time from 00:01 to 99:59.");
        return;
    }
    s_settings_write_in_progress = true;
    s_pending_toggle = (settings_toggle_request_t) {
        .kind = s_time_kind,
        .is_sleep_delay = true,
        .requested_seconds = seconds,
    };
    set_time_picker_busy(true);
    lv_label_set_text(s_time_status, "Saving time...");

    if (xTaskCreate(toggle_apply_task, "sleep_time_save", 6144, NULL, 5, NULL) != pdPASS) {
        s_settings_write_in_progress = false;
        set_time_picker_busy(false);
        lv_label_set_text(s_time_status, "Could not start saving. Try again.");
        lv_obj_set_style_text_color(s_time_status, lv_color_hex(0xDC2626), 0);
    }
}

static lv_obj_t *create_time_roller(
    lv_obj_t *parent, const char *unit, unsigned maximum, uint16_t selected,
    const settings_palette_t *palette)
{
    lv_obj_t *column = lv_obj_create(parent);
    lv_obj_set_size(column, 150, LV_SIZE_CONTENT);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(column, 0, 0);
    lv_obj_set_style_pad_all(column, 0, 0);
    lv_obj_set_style_pad_row(column, 12, 0);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *caption = lv_label_create(column);
    lv_label_set_text(caption, unit);
    lv_obj_set_style_text_color(caption, lv_color_hex(palette->secondary_text), 0);

    /* Every entry is two digits and a newline, except the final terminator. */
    char options[(APP_SLEEP_DELAY_MAX_MINUTES + 1U) * 3U];
    size_t offset = 0;
    for (unsigned value = 0; value <= maximum; value++) {
        const int written = snprintf(options + offset, sizeof(options) - offset,
                                     value == maximum ? "%02u" : "%02u\n", value);
        if (written < 0 || (size_t)written >= sizeof(options) - offset) {
            return NULL;
        }
        offset += (size_t)written;
    }

    lv_obj_t *roller = lv_roller_create(column);
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_width(roller, LV_PCT(100));
    lv_obj_set_style_radius(roller, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(roller, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(roller, lv_color_hex(palette->card_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(roller, lv_color_hex(palette->secondary_text), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(roller, 16, LV_PART_MAIN);
    lv_obj_set_style_text_align(roller, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(roller, lv_color_hex(palette->row_bg), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, lv_color_hex(palette->primary_text), LV_PART_SELECTED);
    lv_obj_set_style_radius(roller, 10, LV_PART_SELECTED);
    lv_obj_set_style_border_width(roller, 1, LV_PART_SELECTED);
    lv_obj_set_style_border_color(roller, lv_color_hex(palette->border), LV_PART_SELECTED);
    lv_roller_set_visible_row_count(roller, 5);
    lv_roller_set_selected(roller, selected, LV_ANIM_OFF);
    lv_obj_remove_flag(roller, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_event_cb(roller, time_roller_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return roller;
}

static lv_obj_t *create_time_action(
    lv_obj_t *parent, const char *text, bool primary,
    lv_event_cb_t callback, const settings_palette_t *palette)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, LV_PCT(47), 58);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button,
        lv_color_hex(primary ? palette->accent : palette->row_bg), 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label,
        primary ? lv_color_white() : lv_color_hex(palette->primary_text), 0);
    lv_obj_center(label);
    return button;
}

static void open_time_picker_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_navigation_dispatched ||
        s_settings_write_in_progress || s_time_overlay != NULL) {
        return;
    }
    s_time_kind = (settings_toggle_kind_t)(uintptr_t)lv_event_get_user_data(event);
    const app_configuration_snapshot_t settings = app_configuration_get();
    const settings_palette_t palette = palette_for(settings.dark_mode);
    const bool light = s_time_kind == SETTINGS_TOGGLE_LIGHT_SLEEP;
    const uint32_t seconds = light ? settings.light_sleep_delay_seconds
                                   : settings.deep_sleep_delay_seconds;
    set_settings_controls_enabled(false);

    s_time_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_time_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_time_overlay);
    lv_obj_remove_flag(s_time_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_time_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(s_time_overlay, 0, 0);
    lv_obj_set_style_border_width(s_time_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_time_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_time_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_time_overlay, LV_OPA_50, 0);
    lv_obj_add_event_cb(s_time_overlay, cancel_time_picker_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sheet = lv_obj_create(s_time_overlay);
    lv_obj_set_size(sheet, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(sheet, 24, 0);
    lv_obj_set_style_border_width(sheet, 0, 0);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(palette.card_bg), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(sheet, 24, 0);
    lv_obj_set_style_pad_row(sheet, 16, 0);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sheet, LV_FLEX_ALIGN_START,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *handle = lv_obj_create(sheet);
    lv_obj_set_size(handle, 64, 5);
    lv_obj_remove_flag(handle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(handle, 0, 0);
    lv_obj_set_style_radius(handle, 3, 0);
    lv_obj_set_style_bg_color(handle, lv_color_hex(palette.border), 0);

    lv_obj_t *title = lv_label_create(sheet);
    lv_label_set_text(title, light ? "Light Sleep time" : "Deep Sleep time");
    lv_obj_set_style_text_color(title, lv_color_hex(palette.primary_text), 0);
    lv_obj_t *description = lv_label_create(sheet);
    lv_label_set_text(description, light
        ? "Time without activity before Active mode enters Light Sleep."
        : settings.light_sleep_enabled
            ? "Time after Light Sleep starts before entering Deep Sleep."
            : "Time without activity before Active mode enters Deep Sleep.");
    lv_obj_set_width(description, LV_PCT(100));
    lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(description, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(description, lv_color_hex(palette.secondary_text), 0);

    lv_obj_t *wheels = lv_obj_create(sheet);
    lv_obj_set_size(wheels, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(wheels, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(wheels, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wheels, 0, 0);
    lv_obj_set_style_pad_all(wheels, 0, 0);
    lv_obj_set_style_pad_column(wheels, 24, 0);
    lv_obj_set_flex_flow(wheels, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wheels, LV_FLEX_ALIGN_CENTER,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_minutes_roller = create_time_roller(wheels, "Minutes", APP_SLEEP_DELAY_MAX_MINUTES,
                                         (uint16_t)(seconds / 60U), &palette);
    s_seconds_roller = create_time_roller(wheels, "Seconds", 59U,
                                         (uint16_t)(seconds % 60U), &palette);
    if (s_minutes_roller == NULL || s_seconds_roller == NULL) {
        close_time_picker();
        set_status("Could not create the time picker.", true);
        return;
    }

    s_time_status = lv_label_create(sheet);
    lv_label_set_text(s_time_status, "Swipe minutes and seconds, then set time.");
    lv_obj_set_width(s_time_status, LV_PCT(100));
    lv_label_set_long_mode(s_time_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_time_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_time_status, lv_color_hex(palette.secondary_text), 0);

    lv_obj_t *actions = lv_obj_create(sheet);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_time_cancel_button = create_time_action(actions, "Cancel", false,
                                               cancel_time_picker_event_cb, &palette);
    s_time_save_button = create_time_action(actions, "Set time", true,
                                             save_time_picker_event_cb, &palette);
    set_time_picker_busy(false);
}

static lv_obj_t *create_toggle_row(
    lv_obj_t *card,
    const char *title,
    const char *detail,
    bool enabled,
    settings_toggle_kind_t kind,
    const settings_palette_t *palette)
{
    const bool has_detail = detail != NULL && detail[0] != '\0';
    const bool is_sleep = kind == SETTINGS_TOGGLE_LIGHT_SLEEP ||
                          kind == SETTINGS_TOGGLE_DEEP_SLEEP;

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_size(row, LV_PCT(100), is_sleep ? 110 : has_detail ? 88 : 70);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *text_box = lv_obj_create(row);
    lv_obj_set_height(text_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text_box, 1);
    lv_obj_remove_flag(text_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(text_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(text_box, 0, 0);
    lv_obj_set_style_pad_all(text_box, 0, 0);
    lv_obj_set_style_pad_row(text_box, 8, 0);
    lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title_label = lv_label_create(text_box);
    lv_label_set_text(title_label, title);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_obj_set_style_text_color(title_label, lv_color_hex(palette->primary_text), 0);

    if (has_detail || is_sleep) {
        lv_obj_t *detail_label = lv_label_create(text_box);
        lv_label_set_text(detail_label, detail != NULL ? detail : "");
        lv_label_set_long_mode(detail_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(detail_label, LV_PCT(100));
        lv_obj_set_style_text_color(detail_label, lv_color_hex(palette->secondary_text), 0);
        if (is_sleep) {
            s_sleep_details[kind == SETTINGS_TOGGLE_LIGHT_SLEEP ? 0 : 1] = detail_label;
        }
    }

    if (is_sleep) {
        const size_t index = kind == SETTINGS_TOGGLE_LIGHT_SLEEP ? 0U : 1U;
        lv_obj_t *button = lv_button_create(row);
        s_time_buttons[index] = button;
        lv_obj_set_size(button, 132, 50);
        lv_obj_set_style_radius(button, 12, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(palette->row_bg), 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(palette->border), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(palette->border), 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(button, open_time_picker_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)kind);
        s_time_labels[index] = lv_label_create(button);
        lv_obj_set_style_text_color(s_time_labels[index],
                                   lv_color_hex(palette->primary_text), 0);
        lv_obj_center(s_time_labels[index]);
    }

    lv_obj_t *toggle = lv_switch_create(row);
    lv_obj_set_size(toggle, 92, 50);
    lv_obj_set_ext_click_area(toggle, 16);
    lv_obj_set_style_bg_color(toggle, lv_color_hex(palette->border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        toggle,
        lv_color_hex(kind == SETTINGS_TOGGLE_ACTIVE_OPTIMIZATION
            ? 0x16A34A : palette->accent),
        LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(toggle, lv_color_white(), LV_PART_KNOB);

    if (enabled) {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }

    lv_obj_add_event_cb(
        toggle,
        toggle_event_cb,
        LV_EVENT_VALUE_CHANGED,
        (void *)(uintptr_t)kind);

    if (s_toggle_count < SETTINGS_TOGGLE_CAPACITY) {
        s_toggles[s_toggle_count++] = toggle;
    }

    return toggle;
}

static void back_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESSED || s_navigation_dispatched) {
        return;
    }

    if (s_time_overlay != NULL) {
        return;
    }
    if (s_settings_write_in_progress) {
        set_status("Please wait for the current setting to finish.", false);
        return;
    }

    s_navigation_dispatched = true;
    ESP_LOGI(TAG, "Back pressed; returning to launcher");

    if (s_back_callback) {
        s_back_callback(s_user_data);
    }
}

void settings_screen_create(
    settings_screen_back_callback_t back_callback,
    void *user_data)
{
    s_back_callback = back_callback;
    s_user_data = user_data;
    s_navigation_dispatched = false;
    s_switch_update_in_progress = false;
    s_settings_write_in_progress = false;
    s_last_toggle_event_us = 0;
    s_back_button = NULL;
    s_toggle_count = 0;
    s_time_overlay = NULL;
    s_minutes_roller = NULL;
    s_seconds_roller = NULL;
    s_time_save_button = NULL;
    s_time_cancel_button = NULL;
    s_time_status = NULL;
    for (size_t index = 0; index < 2; index++) {
        s_time_buttons[index] = NULL;
        s_time_labels[index] = NULL;
        s_sleep_details[index] = NULL;
    }

    /* Required fresh read: this function is called for every Settings press. */
    app_configuration_authorized_users_t users;
    const esp_err_t users_ret = app_configuration_load_authorized_users(&users);
    const app_configuration_snapshot_t settings = app_configuration_get();
    const settings_palette_t palette = palette_for(settings.dark_mode);

    if (settings_screen_lock() != ESP_OK) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);

    /*
     * The Settings list uses flat opaque surfaces to minimize redraw work.
     * The time picker adds a temporary dimmed backdrop and native rollers.
     */
    lv_obj_set_style_bg_color(screen, lv_color_hex(palette.screen_bg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, LV_PCT(100), 112);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(palette.header_bg), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);

    s_back_button = lv_button_create(header);
    lv_obj_set_size(s_back_button, 76, 62);
    lv_obj_align(s_back_button, LV_ALIGN_LEFT_MID, 28, 0);
    lv_obj_set_style_radius(s_back_button, 20, 0);
    lv_obj_set_style_bg_color(s_back_button, lv_color_hex(palette.card_bg), 0);
    lv_obj_set_style_bg_color(
        s_back_button,
        lv_color_hex(palette.row_bg),
        LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_back_button, lv_color_hex(palette.border), 0);
    lv_obj_set_style_border_width(s_back_button, 1, 0);
    lv_obj_add_event_cb(s_back_button, back_event_cb, LV_EVENT_PRESSED, NULL);

    lv_obj_t *back_label = lv_label_create(s_back_button);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_hex(palette.primary_text), 0);
    lv_obj_center(back_label);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(palette.primary_text), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(91));
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_top(content, 30, 0);
    lv_obj_set_style_pad_bottom(content, 44, 0);
    lv_obj_set_style_pad_row(content, 14, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        content,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);

    create_authorized_users(content, &palette, &users, users_ret);

    create_section_label(content, "THEME", &palette);
    lv_obj_t *theme_card = create_card(content, &palette);
    create_toggle_row(
        theme_card,
        "Dark Mode",
        "Light mode is the default",
        settings.dark_mode,
        SETTINGS_TOGGLE_THEME,
        &palette);

    create_section_label(content, "POWER MODES", &palette);
    lv_obj_t *power_modes_card = create_card(content, &palette);
    create_toggle_row(
        power_modes_card,
        "Active Mode Optimization",
        NULL,
        settings.active_optimization_enabled,
        SETTINGS_TOGGLE_ACTIVE_OPTIMIZATION,
        &palette);
    create_divider(power_modes_card, &palette);
    create_toggle_row(
        power_modes_card,
        "Light Sleep",
        NULL,
        settings.light_sleep_enabled,
        SETTINGS_TOGGLE_LIGHT_SLEEP,
        &palette);
    create_divider(power_modes_card, &palette);
    create_toggle_row(
        power_modes_card,
        "Deep Sleep",
        NULL,
        settings.deep_sleep_enabled,
        SETTINGS_TOGGLE_DEEP_SLEEP,
        &palette);

    refresh_sleep_labels(&settings);

    s_status_label = lv_label_create(content);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status_label, LV_PCT(86));
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(s_status_label, 8, 0);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);

    if (users_ret != ESP_OK) {
        set_status("Authorized-user refresh failed. Settings remain available.", true);
    }

    bsp_display_unlock();
}

void settings_screen_destroy(void)
{
    if (settings_screen_lock() != ESP_OK) {
        return;
    }

    lv_obj_clean(lv_screen_active());
    s_status_label = NULL;
    s_back_button = NULL;
    s_toggle_count = 0;
    s_time_overlay = NULL;
    s_minutes_roller = NULL;
    s_seconds_roller = NULL;
    s_time_save_button = NULL;
    s_time_cancel_button = NULL;
    s_time_status = NULL;
    for (size_t index = 0; index < 2; index++) {
        s_time_buttons[index] = NULL;
        s_time_labels[index] = NULL;
        s_sleep_details[index] = NULL;
    }
    s_back_callback = NULL;
    s_user_data = NULL;
    s_navigation_dispatched = false;
    s_switch_update_in_progress = false;
    s_settings_write_in_progress = false;

    bsp_display_unlock();
}
