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
#include "settings/app_settings.h"

typedef enum {
    SETTINGS_TOGGLE_THEME,
    SETTINGS_TOGGLE_ETHERNET,
    SETTINGS_TOGGLE_WIFI,
    SETTINGS_TOGGLE_CAMERA,
    SETTINGS_TOGGLE_AUDIO,
    SETTINGS_TOGGLE_SDCARD,
    SETTINGS_TOGGLE_LIGHT_SLEEP,
    SETTINGS_TOGGLE_DEEP_SLEEP,
} settings_toggle_kind_t;

#define SETTINGS_TOGGLE_CAPACITY       8U
#define SETTINGS_TOGGLE_DEBOUNCE_US    250000LL

typedef struct {
    lv_obj_t *toggle;
    settings_toggle_kind_t kind;
    bool requested_enabled;
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
static volatile bool s_toggle_write_in_progress;
static int64_t s_last_toggle_event_us;
static settings_toggle_request_t s_pending_toggle;

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
    lv_obj_set_style_radius(card, 26, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(palette->card_bg), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(palette->border), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_shadow_opa(card, palette->dark ? LV_OPA_20 : LV_OPA_10, 0);
    lv_obj_set_style_shadow_width(card, 16, 0);
    lv_obj_set_style_shadow_ofs_y(card, 5, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
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
    const app_settings_authorized_users_t *users,
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
    const app_settings_snapshot_t *settings,
    settings_toggle_kind_t kind)
{
    switch (kind) {
    case SETTINGS_TOGGLE_THEME:
        return settings->dark_mode;
    case SETTINGS_TOGGLE_ETHERNET:
        return settings->ethernet_enabled;
    case SETTINGS_TOGGLE_WIFI:
        return settings->wifi_enabled;
    case SETTINGS_TOGGLE_CAMERA:
        return settings->camera_enabled;
    case SETTINGS_TOGGLE_AUDIO:
        return settings->audio_enabled;
    case SETTINGS_TOGGLE_SDCARD:
        return settings->sdcard_enabled;
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
        return app_settings_set_dark_mode(enabled);
    case SETTINGS_TOGGLE_ETHERNET:
        return app_settings_set_ethernet_enabled(enabled);
    case SETTINGS_TOGGLE_WIFI:
        return app_settings_set_wifi_enabled(enabled);
    case SETTINGS_TOGGLE_CAMERA:
        return app_settings_set_camera_enabled(enabled);
    case SETTINGS_TOGGLE_AUDIO:
        return app_settings_set_audio_enabled(enabled);
    case SETTINGS_TOGGLE_SDCARD:
        return app_settings_set_sdcard_enabled(enabled);
    case SETTINGS_TOGGLE_LIGHT_SLEEP:
        return app_settings_set_light_sleep_enabled(enabled);
    case SETTINGS_TOGGLE_DEEP_SLEEP:
        return app_settings_set_deep_sleep_enabled(enabled);
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
    case SETTINGS_TOGGLE_ETHERNET:
        return enabled
            ? "Ethernet enabled and saved."
            : "Ethernet powered down and saved.";
    case SETTINGS_TOGGLE_WIFI:
        return enabled
            ? "Wi-Fi coprocessor enabled and saved."
            : "Wi-Fi coprocessor disabled and saved.";
    case SETTINGS_TOGGLE_CAMERA:
        return enabled
            ? "Camera enabled; Start Camera is available."
            : "Camera disabled; its pipeline will remain off.";
    case SETTINGS_TOGGLE_AUDIO:
        return enabled
            ? "Audio codec and amplifier enabled and saved."
            : "Audio codec and amplifier powered down and saved.";
    case SETTINGS_TOGGLE_SDCARD:
        return enabled
            ? "microSD power enabled, mounted, and saved."
            : "microSD powers down after required SD access.";
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
    const esp_err_t ret = apply_toggle_setting(
        request.kind,
        request.requested_enabled);
    const app_settings_snapshot_t settings = app_settings_get();
    const bool saved_enabled = setting_value_for_kind(&settings, request.kind);
    const bool rebuild_theme =
        ret == ESP_OK && request.kind == SETTINGS_TOGGLE_THEME;

    if (settings_screen_lock() == ESP_OK) {
        sync_toggle_state(request.toggle, saved_enabled);

        if (ret == ESP_OK) {
            set_status(
                toggle_success_message(request.kind, saved_enabled),
                false);
        } else {
            set_status(
                "Change failed and was not saved. Check the serial log.",
                true);
        }

        if (!rebuild_theme) {
            set_settings_controls_enabled(true);
            s_toggle_write_in_progress = false;
        }

        bsp_display_unlock();
    } else {
        s_toggle_write_in_progress = false;
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

    if (s_toggle_write_in_progress ||
        (s_last_toggle_event_us != 0 &&
         now_us - s_last_toggle_event_us < SETTINGS_TOGGLE_DEBOUNCE_US)) {
        const app_settings_snapshot_t settings = app_settings_get();
        sync_toggle_state(toggle, setting_value_for_kind(&settings, kind));
        return;
    }

    s_last_toggle_event_us = now_us;
    s_toggle_write_in_progress = true;
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
        const app_settings_snapshot_t settings = app_settings_get();
        sync_toggle_state(toggle, setting_value_for_kind(&settings, kind));
        set_settings_controls_enabled(true);
        s_toggle_write_in_progress = false;
        set_status("Could not start the settings worker. Try again.", true);
    }
}

static lv_obj_t *create_toggle_row(
    lv_obj_t *card,
    const char *title,
    const char *detail,
    bool enabled,
    settings_toggle_kind_t kind,
    const settings_palette_t *palette)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_size(row, LV_PCT(100), 92);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(row, 4, 0);
    lv_obj_set_style_pad_right(row, 4, 0);

    lv_obj_t *title_label = lv_label_create(row);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(palette->primary_text), 0);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 0, -13);

    lv_obj_t *detail_label = lv_label_create(row);
    lv_label_set_text(detail_label, detail);
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(detail_label, LV_PCT(70));
    lv_obj_set_style_text_color(detail_label, lv_color_hex(palette->secondary_text), 0);
    lv_obj_align(detail_label, LV_ALIGN_LEFT_MID, 0, 16);

    lv_obj_t *toggle = lv_switch_create(row);
    lv_obj_set_size(toggle, 96, 52);
    lv_obj_set_ext_click_area(toggle, 16);
    lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(toggle, lv_color_hex(palette->border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        toggle,
        lv_color_hex(palette->accent),
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

    if (s_toggle_write_in_progress) {
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
    s_toggle_write_in_progress = false;
    s_last_toggle_event_us = 0;
    s_back_button = NULL;
    s_toggle_count = 0;

    /* Required fresh read: this function is called for every Settings press. */
    app_settings_authorized_users_t users;
    const esp_err_t users_ret = app_settings_load_authorized_users(&users);
    const app_settings_snapshot_t settings = app_settings_get();
    const settings_palette_t palette = palette_for(settings.dark_mode);

    if (settings_screen_lock() != ESP_OK) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
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
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *subtitle = lv_label_create(header);
    lv_label_set_text(subtitle, "Device preferences and power policy");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(palette.secondary_text), 0);
    lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 20);

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
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_ACTIVE);
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

    create_section_label(content, "POWER MANAGEMENT", &palette);
    lv_obj_t *power_card = create_card(content, &palette);
    create_toggle_row(
        power_card,
        "Ethernet",
        "IP101GRI PHY",
        settings.ethernet_enabled,
        SETTINGS_TOGGLE_ETHERNET,
        &palette);
    create_divider(power_card, &palette);
    create_toggle_row(
        power_card,
        "Wi-Fi",
        "ESP32-C6 coprocessor",
        settings.wifi_enabled,
        SETTINGS_TOGGLE_WIFI,
        &palette);
    create_divider(power_card, &palette);
    create_toggle_row(
        power_card,
        "Camera",
        "OV5647 capture and AI pipeline",
        settings.camera_enabled,
        SETTINGS_TOGGLE_CAMERA,
        &palette);
    create_divider(power_card, &palette);
    create_toggle_row(
        power_card,
        "Audio",
        "ES8311 codec and NS4150B amplifier",
        settings.audio_enabled,
        SETTINGS_TOGGLE_AUDIO,
        &palette);
    create_divider(power_card, &palette);
    create_toggle_row(
        power_card,
        "microSD",
        "GPIO45-controlled SD1_VDD rail",
        settings.sdcard_enabled,
        SETTINGS_TOGGLE_SDCARD,
        &palette);

    create_section_label(content, "POWER MODES", &palette);
    lv_obj_t *power_modes_card = create_card(content, &palette);
    create_toggle_row(
        power_modes_card,
        "Light Sleep",
        "Reversible; touch or GPIO3 returns to Active",
        settings.light_sleep_enabled,
        SETTINGS_TOGGLE_LIGHT_SLEEP,
        &palette);
    create_divider(power_modes_card, &palette);
    create_toggle_row(
        power_modes_card,
        "Deep Sleep",
        "When both are ON, starts 10 s after Light Sleep",
        settings.deep_sleep_enabled,
        SETTINGS_TOGGLE_DEEP_SLEEP,
        &palette);

    s_status_label = lv_label_create(content);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status_label, LV_PCT(86));
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(s_status_label, 8, 0);

    if (users_ret == ESP_OK) {
        char refresh_status[128];
        snprintf(
            refresh_status,
            sizeof(refresh_status),
            settings.sdcard_enabled
                ? "%u authorized user%s refreshed from microSD."
                : "%u authorized user%s refreshed; microSD returned to OFF.",
            (unsigned)users.count,
            users.count == 1 ? "" : "s");
        set_status(refresh_status, false);
    } else {
        set_status("Authorized-user refresh failed. Power settings remain available.", true);
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
    s_back_callback = NULL;
    s_user_data = NULL;
    s_navigation_dispatched = false;
    s_switch_update_in_progress = false;
    s_toggle_write_in_progress = false;

    bsp_display_unlock();
}
