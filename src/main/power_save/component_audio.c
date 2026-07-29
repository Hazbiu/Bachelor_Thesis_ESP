#include "power_save/component_audio.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#define AUDIO_POWER_AMP_GPIO GPIO_NUM_53
#define AUDIO_AMP_ENABLED    1
#define AUDIO_AMP_DISABLED   0

static const char *TAG = "component_audio";

static bool s_audio_amp_disabled;

esp_err_t component_audio_disable_for_deep_sleep(void)
{
    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << AUDIO_POWER_AMP_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_config);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "GPIO%d configuration failed: %s",
            AUDIO_POWER_AMP_GPIO,
            esp_err_to_name(ret));

        return ret;
    }

    ret = gpio_set_level(
        AUDIO_POWER_AMP_GPIO,
        AUDIO_AMP_DISABLED);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not disable audio amplifier: %s",
            esp_err_to_name(ret));

        return ret;
    }

    ret = gpio_hold_en(AUDIO_POWER_AMP_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not hold GPIO%d LOW: %s",
            AUDIO_POWER_AMP_GPIO,
            esp_err_to_name(ret));

        return ret;
    }

    s_audio_amp_disabled = true;

    ESP_LOGI(
        TAG,
        "Audio amplifier disabled: GPIO%d held LOW",
        AUDIO_POWER_AMP_GPIO);

    return ESP_OK;
}

esp_err_t component_audio_restore_after_failed_sleep(void)
{
    if (!s_audio_amp_disabled) {
        return ESP_OK;
    }

    esp_err_t first_error = ESP_OK;

    esp_err_t ret = gpio_hold_dis(AUDIO_POWER_AMP_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not release GPIO%d hold: %s",
            AUDIO_POWER_AMP_GPIO,
            esp_err_to_name(ret));

        first_error = ret;
    }

    ret = gpio_set_level(
        AUDIO_POWER_AMP_GPIO,
        AUDIO_AMP_ENABLED);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not restore audio amplifier: %s",
            esp_err_to_name(ret));

        if (first_error == ESP_OK) {
            first_error = ret;
        }
    } else {
        ESP_LOGW(
            TAG,
            "Deep sleep failed; audio amplifier restored");
    }

    s_audio_amp_disabled = false;
    return first_error;
}
