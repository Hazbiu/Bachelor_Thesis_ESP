#include "settings_screen.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "settings/app_settings.h"

typedef enum {
    SETTINGS_TOGGLE_THEME,
    SETTINGS_TOGGLE_ETHERNET,
    SETTINGS_TOGGLE_WIFI,
} settings_toggle_kind_t;

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
static bool s_navigation_dispatched;
static bool s_switch_update_in_progress;

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

static void theme_rebuild_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(40));
    settings_screen_create(s_back_callback, s_user_data);
    vTaskDelete(NULL);
}

static void toggle_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED ||
        s_switch_update_in_progress) {
        return;
    }

    lv_obj_t *toggle = lv_event_get_target_obj(event);
    const bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    const settings_toggle_kind_t kind =
        (settings_toggle_kind_t)(uintptr_t)lv_event_get_user_data(event);

    esp_err_t ret = ESP_OK;
    const char *success_message = NULL;

    switch (kind) {
    case SETTINGS_TOGGLE_THEME:
        ret = app_settings_set_dark_mode(enabled);
        success_message = enabled ? "Dark mode saved." : "Light mode saved.";
        break;
    case SETTINGS_TOGGLE_ETHERNET:
        ret = app_settings_set_ethernet_enabled(enabled);
        success_message = enabled
            ? "Ethernet enabled and saved."
            : "Ethernet powered down and saved.";
        break;
    case SETTINGS_TOGGLE_WIFI:
        ret = app_settings_set_wifi_enabled(enabled);
        success_message = enabled
            ? "Wi-Fi coprocessor enabled and saved."
            : "Wi-Fi coprocessor disabled and saved.";
        break;
    }

    if (ret != ESP_OK) {
        s_switch_update_in_progress = true;
        if (enabled) {
            lv_obj_remove_state(toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        }
        s_switch_update_in_progress = false;
        set_status("Change failed and was not saved. Check the serial log.", true);
        return;
    }

    set_status(success_message, false);

    if (kind == SETTINGS_TOGGLE_THEME) {
        BaseType_t created = xTaskCreate(
            theme_rebuild_task,
            "settings_theme",
            4096,
            NULL,
            5,
            NULL);
        if (created != pdPASS) {
            set_status("Theme saved. Reopen Settings to refresh the colors.", true);
        }
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
    lv_obj_set_size(row, LV_PCT(100), 86);
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
    lv_obj_set_style_text_color(detail_label, lv_color_hex(palette->secondary_text), 0);
    lv_obj_align(detail_label, LV_ALIGN_LEFT_MID, 0, 16);

    lv_obj_t *toggle = lv_switch_create(row);
    lv_obj_set_size(toggle, 86, 46);
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
    return toggle;
}

static void back_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESSED || s_navigation_dispatched) {
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

    lv_obj_t *back_button = lv_button_create(header);
    lv_obj_set_size(back_button, 76, 62);
    lv_obj_align(back_button, LV_ALIGN_LEFT_MID, 28, 0);
    lv_obj_set_style_radius(back_button, 20, 0);
    lv_obj_set_style_bg_color(back_button, lv_color_hex(palette.card_bg), 0);
    lv_obj_set_style_bg_color(back_button, lv_color_hex(palette.row_bg), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(back_button, lv_color_hex(palette.border), 0);
    lv_obj_set_style_border_width(back_button, 1, 0);
    lv_obj_add_event_cb(back_button, back_event_cb, LV_EVENT_PRESSED, NULL);

    lv_obj_t *back_label = lv_label_create(back_button);
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

    s_status_label = lv_label_create(content);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status_label, LV_PCT(86));
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(s_status_label, 8, 0);

    if (users_ret == ESP_OK) {
        char refresh_status[96];
        snprintf(
            refresh_status,
            sizeof(refresh_status),
            "%u authorized user%s refreshed from microSD.",
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
    s_back_callback = NULL;
    s_user_data = NULL;
    s_navigation_dispatched = false;
    s_switch_update_in_progress = false;

    bsp_display_unlock();
}
