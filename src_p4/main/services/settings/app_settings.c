
#include "services/settings/app_settings.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define SETTINGS_NAMESPACE "app_settings"
#define KEY_DARK_MODE       "dark"
#define KEY_ETHERNET        "ethernet"
#define KEY_WIFI            "wifi"
#define KEY_CAMERA          "camera"
#define KEY_AUDIO           "audio"
#define KEY_SDCARD          "sdcard"
#define KEY_ACTIVE_OPT      "active_opt"
#define KEY_LIGHT_SLEEP     "light_sleep"
#define KEY_DEEP_SLEEP      "deep_sleep"
#define ENROLLMENT_ROOT     "/sdcard/enroll"

static const char *TAG = "app_settings";

static app_settings_snapshot_t s_settings = {
    .dark_mode = false,
    .ethernet_enabled = true,
    .wifi_enabled = true,
    .camera_enabled = true,
    .audio_enabled = true,
    .sdcard_enabled = true,
    /* Missing key on an existing device preserves PWR-OPT-3 behavior. */
    .active_optimization_enabled = true,
    .light_sleep_enabled = true,
    .deep_sleep_enabled = true,
};

static bool s_initialized;
static bool s_nvs_available;

static esp_err_t read_bool(nvs_handle_t handle, const char *key, bool *value)
{
    uint8_t stored = 0;
    esp_err_t ret = nvs_get_u8(handle, key, &stored);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    *value = stored != 0;
    return ESP_OK;
}

static esp_err_t save_bool(const char *key, bool value)
{
    if (!s_nvs_available) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(handle, key, value ? 1U : 0U);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

esp_err_t app_settings_init(void)
{
    if (s_initialized) {
        return s_nvs_available ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    s_initialized = true;

    esp_err_t ret = nvs_flash_init();
    if (ret != ESP_OK) {
        /* Preserve other NVS users; never erase the partition automatically. */
        ESP_LOGE(
            TAG,
            "NVS initialization failed; defaults remain active: %s",
            esp_err_to_name(ret));
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace is created on the first user change. */
        s_nvs_available = true;
        ESP_LOGI(TAG, "No saved settings; using light mode with all components enabled");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not open settings namespace: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_err_t first_error = ESP_OK;
    esp_err_t item_ret = read_bool(handle, KEY_DARK_MODE, &s_settings.dark_mode);
    if (item_ret != ESP_OK) {
        first_error = item_ret;
    }

    item_ret = read_bool(handle, KEY_ETHERNET, &s_settings.ethernet_enabled);
    if (item_ret != ESP_OK && first_error == ESP_OK) {
        first_error = item_ret;
    }

    item_ret = read_bool(handle, KEY_WIFI, &s_settings.wifi_enabled);
    if (item_ret != ESP_OK && first_error == ESP_OK) {
        first_error = item_ret;
    }

    item_ret = read_bool(handle, KEY_CAMERA, &s_settings.camera_enabled);
    if (item_ret != ESP_OK && first_error == ESP_OK) {
        first_error = item_ret;
    }

    item_ret = read_bool(handle, KEY_AUDIO, &s_settings.audio_enabled);
    if (item_ret != ESP_OK && first_error == ESP_OK) {
        first_error = item_ret;
    }

    item_ret = read_bool(handle, KEY_SDCARD, &s_settings.sdcard_enabled);
    if (item_ret != ESP_OK && first_error == ESP_OK) {
        first_error = item_ret;
    }

    item_ret = read_bool(
        handle,
        KEY_ACTIVE_OPT,
        &s_settings.active_optimization_enabled);
    if (item_ret != ESP_OK && first_error == ESP_OK) {
        first_error = item_ret;
    }

    item_ret = read_bool(
        handle,
        KEY_LIGHT_SLEEP,
        &s_settings.light_sleep_enabled);
    if (item_ret != ESP_OK && first_error == ESP_OK) {
        first_error = item_ret;
    }

    item_ret = read_bool(
        handle,
        KEY_DEEP_SLEEP,
        &s_settings.deep_sleep_enabled);
    if (item_ret != ESP_OK && first_error == ESP_OK) {
        first_error = item_ret;
    }

    nvs_close(handle);
    s_nvs_available = first_error == ESP_OK;

    if (first_error != ESP_OK) {
        ESP_LOGE(TAG, "Could not read one or more settings: %s",
                 esp_err_to_name(first_error));
        return first_error;
    }

    ESP_LOGI(
        TAG,
        "Loaded settings: theme=%s ethernet=%s wifi=%s camera=%s audio=%s "
        "sdcard=%s active_optimization=%s light_sleep=%s deep_sleep=%s",
        s_settings.dark_mode ? "dark" : "light",
        s_settings.ethernet_enabled ? "on" : "off",
        s_settings.wifi_enabled ? "on" : "off",
        s_settings.camera_enabled ? "on" : "off",
        s_settings.audio_enabled ? "on" : "off",
        s_settings.sdcard_enabled ? "on" : "off",
        s_settings.active_optimization_enabled ? "on" : "off",
        s_settings.light_sleep_enabled ? "on" : "off",
        s_settings.deep_sleep_enabled ? "on" : "off");
    return ESP_OK;
}

app_settings_snapshot_t app_settings_get(void)
{
    return s_settings;
}

esp_err_t app_settings_set_dark_mode(bool enabled)
{
    const bool previous = s_settings.dark_mode;
    s_settings.dark_mode = enabled;

    esp_err_t ret = save_bool(KEY_DARK_MODE, enabled);
    if (ret != ESP_OK) {
        s_settings.dark_mode = previous;
        ESP_LOGE(TAG, "Could not save theme: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Theme saved: %s", enabled ? "dark" : "light");
    return ESP_OK;
}

esp_err_t app_settings_set_ethernet_enabled(bool enabled)
{
    const bool previous = s_settings.ethernet_enabled;
    if (enabled == previous) {
        return ESP_OK;
    }

    const esp_err_t ret = save_bool(KEY_ETHERNET, enabled);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not save Ethernet setting: %s", esp_err_to_name(ret));
        return ret;
    }

    s_settings.ethernet_enabled = enabled;
    ESP_LOGI(TAG, "Ethernet saved: %s", enabled ? "on" : "off");
    return ESP_OK;
}

esp_err_t app_settings_set_wifi_enabled(bool enabled)
{
    const bool previous = s_settings.wifi_enabled;
    if (enabled == previous) {
        return ESP_OK;
    }

    const esp_err_t ret = save_bool(KEY_WIFI, enabled);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not save Wi-Fi setting: %s", esp_err_to_name(ret));
        return ret;
    }

    s_settings.wifi_enabled = enabled;
    ESP_LOGI(TAG, "Wi-Fi saved: %s", enabled ? "on" : "off");
    return ESP_OK;
}

esp_err_t app_settings_set_camera_enabled(bool enabled)
{
    const bool previous = s_settings.camera_enabled;
    if (enabled == previous) {
        return ESP_OK;
    }

    esp_err_t ret = save_bool(KEY_CAMERA, enabled);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not save Camera setting: %s", esp_err_to_name(ret));
        return ret;
    }

    s_settings.camera_enabled = enabled;
    ESP_LOGI(
        TAG,
        "Camera policy saved: %s; camera pipeline is %s",
        enabled ? "on" : "off",
        enabled ? "available" : "blocked and uninitialized");
    return ESP_OK;
}

esp_err_t app_settings_set_audio_enabled(bool enabled)
{
    const bool previous = s_settings.audio_enabled;
    if (enabled == previous) {
        return ESP_OK;
    }

    const esp_err_t ret = save_bool(KEY_AUDIO, enabled);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not save Audio setting: %s", esp_err_to_name(ret));
        return ret;
    }

    s_settings.audio_enabled = enabled;
    ESP_LOGI(TAG, "Audio saved: %s", enabled ? "on" : "off");
    return ESP_OK;
}

esp_err_t app_settings_set_sdcard_enabled(bool enabled)
{
    const bool previous = s_settings.sdcard_enabled;
    if (enabled == previous) {
        return ESP_OK;
    }

    const esp_err_t ret = save_bool(KEY_SDCARD, enabled);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not save microSD setting: %s", esp_err_to_name(ret));
        return ret;
    }

    s_settings.sdcard_enabled = enabled;
    ESP_LOGI(TAG, "microSD saved: %s", enabled ? "on" : "off");
    return ESP_OK;
}

esp_err_t app_settings_set_active_optimization_enabled(bool enabled)
{
    if (s_settings.active_optimization_enabled == enabled) {
        return ESP_OK;
    }

    const esp_err_t ret = save_bool(KEY_ACTIVE_OPT, enabled);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not save Active Mode Optimization: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    s_settings.active_optimization_enabled = enabled;
    ESP_LOGI(TAG, "Active Mode Optimization saved: %s",
             enabled ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t app_settings_set_light_sleep_enabled(bool enabled)
{
    const bool previous = s_settings.light_sleep_enabled;
    if (enabled == previous) {
        return ESP_OK;
    }

    const esp_err_t ret = save_bool(KEY_LIGHT_SLEEP, enabled);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not save Light-sleep setting: %s", esp_err_to_name(ret));
        return ret;
    }

    s_settings.light_sleep_enabled = enabled;
    ESP_LOGI(TAG, "Light-sleep mode saved: %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t app_settings_set_deep_sleep_enabled(bool enabled)
{
    const bool previous = s_settings.deep_sleep_enabled;
    if (enabled == previous) {
        return ESP_OK;
    }

    const esp_err_t ret = save_bool(KEY_DEEP_SLEEP, enabled);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not save Deep-sleep setting: %s", esp_err_to_name(ret));
        return ret;
    }

    s_settings.deep_sleep_enabled = enabled;
    ESP_LOGI(TAG, "Deep-sleep mode saved: %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

static bool is_directory_entry(const struct dirent *entry)
{
    /* FAT directory entries are not required to expose d_type; use stat. */
    char full_path[256];
    int written = snprintf(full_path, sizeof(full_path), "%s/%s",
                           ENROLLMENT_ROOT, entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(full_path)) {
        return false;
    }

    struct stat info;
    return stat(full_path, &info) == 0 && S_ISDIR(info.st_mode);
}

static int compare_names(const void *left, const void *right)
{
    const char *left_name = left;
    const char *right_name = right;
    return strcasecmp(left_name, right_name);
}

esp_err_t app_settings_load_authorized_users(
    app_settings_authorized_users_t *users_out)
{
    if (users_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(users_out, 0, sizeof(*users_out));

    /*
     * Configuration Service only scans configuration data. Application Logic
     * is responsible for temporarily enabling/mounting microSD when the saved
     * storage policy is OFF.
     */
    DIR *directory = opendir(ENROLLMENT_ROOT);
    if (directory == NULL) {
        const esp_err_t scan_ret = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        ESP_LOGE(TAG, "Could not open %s: errno=%d", ENROLLMENT_ROOT, errno);
        return scan_ret;
    }

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL &&
           users_out->count < APP_SETTINGS_MAX_AUTHORIZED_USERS) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            entry->d_name[0] == '.') {
            continue;
        }

        if (!is_directory_entry(entry)) {
            continue;
        }

        char *destination = users_out->names[users_out->count];
        const size_t name_length = strnlen(
            entry->d_name,
            APP_SETTINGS_MAX_USER_NAME_LENGTH - 1U);
        memcpy(destination, entry->d_name, name_length);
        destination[name_length] = '\0';
        users_out->count++;
    }

    closedir(directory);

    qsort(
        users_out->names,
        users_out->count,
        APP_SETTINGS_MAX_USER_NAME_LENGTH,
        compare_names);

    ESP_LOGI(TAG, "Authorized users refreshed from SD card: %u",
             (unsigned)users_out->count);

    return ESP_OK;
}
