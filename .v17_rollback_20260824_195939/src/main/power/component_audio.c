#include "power_save/component_audio.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "config/app_config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_POWER_AMP_GPIO APP_PWR_AUDIO_AMP_GPIO
#define AUDIO_AMP_ENABLED    APP_PWR_AUDIO_AMP_ENABLED_LEVEL
#define AUDIO_AMP_DISABLED   APP_PWR_AUDIO_AMP_DISABLED_LEVEL

static const char *TAG = "component_audio";

static bool s_audio_amp_disabled;

/* See component_wifi.c: a held pad may need time before it reads back. */
static int wait_for_pad_level(gpio_num_t gpio_num, int expected_level,
                              uint32_t *settle_ms_out)
{
    uint32_t waited_ms = 0;
    int level = gpio_get_level(gpio_num);

    while (level != expected_level && waited_ms < APP_PWR_HOLD_VERIFY_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(APP_PWR_HOLD_VERIFY_POLL_MS));
        waited_ms += APP_PWR_HOLD_VERIFY_POLL_MS;
        level = gpio_get_level(gpio_num);
    }

    if (settle_ms_out != NULL) {
        *settle_ms_out = waited_ms;
    }

    return level;
}

esp_err_t component_audio_disable_for_deep_sleep(void)
{
    /*
     * This low-level rail operation is also reused by the reversible
     * Light-sleep path. Keep it idempotent so the later Deep-sleep sequence can
     * call it again without disturbing an already-held LOW pad.
     */
    if (s_audio_amp_disabled) {
        return ESP_OK;
    }

    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << AUDIO_POWER_AMP_GPIO,
        /* INPUT_OUTPUT so the pre-sleep rail audit can read the pad back. */
        .mode = GPIO_MODE_INPUT_OUTPUT,
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

    /* Keep the active OUTPUT-LOW configuration across the sleep boundary. */
    ret = gpio_sleep_sel_dis(AUDIO_POWER_AMP_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not disable sleep switching for GPIO%d: %s",
            AUDIO_POWER_AMP_GPIO,
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

    uint32_t settle_ms = 0;
    const int level_after_hold = wait_for_pad_level(
        AUDIO_POWER_AMP_GPIO,
        AUDIO_AMP_DISABLED,
        &settle_ms);

    if (level_after_hold != AUDIO_AMP_DISABLED) {
        ESP_LOGE(
            TAG,
            "Audio amplifier EN still reads %d after holding for %" PRIu32
            " ms (expected %d)",
            level_after_hold,
            settle_ms,
            AUDIO_AMP_DISABLED);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "Audio amplifier disabled: GPIO%d held LOW "
        "(read-back=%d after %" PRIu32 " ms)",
        AUDIO_POWER_AMP_GPIO,
        level_after_hold,
        settle_ms);

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
        ESP_LOGI(TAG, "Audio amplifier restored after sleep");
    }

    s_audio_amp_disabled = false;
    return first_error;
}
