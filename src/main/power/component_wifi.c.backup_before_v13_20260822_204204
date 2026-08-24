#include "power_save/component_wifi.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "config/app_config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Waveshare ESP32-P4-NANO connection:
 *
 * ESP32-P4 GPIO54 -> R54 -> ESP32-C6 CHIP_PU
 *
 * LOW  = ESP32-C6 held in reset
 * HIGH = ESP32-C6 enabled
 */
#define WIFI_C6_CHIP_PU_GPIO    APP_PWR_WIFI_C6_CHIP_PU_GPIO
#define WIFI_C6_DISABLED_LEVEL  APP_PWR_WIFI_C6_DISABLED_LEVEL
#define WIFI_C6_ENABLED_LEVEL   APP_PWR_WIFI_C6_ENABLED_LEVEL

static const char *TAG = "component_wifi";

static bool s_c6_disabled;

/*
 * Poll the pad until it reaches the expected level, or until the settle
 * window expires.
 *
 * A pad does not always report its new level on the instruction immediately
 * after gpio_hold_en(). On this board GPIO54 reads HIGH right after the hold
 * and LOW a few tens of milliseconds later, which previously produced a false
 * "coprocessor is still powered" error even though the pre-sleep rail audit
 * then reported the pin correctly held LOW.
 *
 * Returns the last level read and reports how long the pad took to settle.
 */
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

esp_err_t component_wifi_disable_for_deep_sleep(void)
{
    if (s_c6_disabled) {
        return ESP_OK;
    }
    const gpio_num_t pin = WIFI_C6_CHIP_PU_GPIO;

    /*
     * Make sure GPIO54 is not carrying an old hold state.
     */
    esp_err_t ret = gpio_hold_dis(pin);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(
            TAG,
            "Could not release previous GPIO%d hold: %s",
            pin,
            esp_err_to_name(ret));
        return ret;
    }

    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << pin,

        /*
         * Keep input enabled so gpio_get_level() and the
         * pre-Deep-sleep audit can still measure the pad.
         */
        .mode = GPIO_MODE_INPUT_OUTPUT,

        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&io_config);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "GPIO%d configuration failed: %s",
            pin,
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * Use maximum output drive strength.
     */
    ret = gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_3);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not set GPIO%d drive strength: %s",
            pin,
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * C6 CHIP_PU:
     * LOW = C6 disabled
     */
    ret = gpio_set_level(pin, WIFI_C6_DISABLED_LEVEL);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not drive ESP32-C6 CHIP_PU LOW: %s",
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * Keep the active GPIO configuration during sleep.
     */
    ret = gpio_sleep_sel_dis(pin);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not disable sleep switching for GPIO%d: %s",
            pin,
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * Latch output-enable, output level, function and drive strength.
     */
    ret = gpio_hold_en(pin);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not hold GPIO%d LOW: %s",
            pin,
            esp_err_to_name(ret));
        return ret;
    }

    s_c6_disabled = true;

    uint32_t settle_ms = 0;
    const int level_after_hold = wait_for_pad_level(
        pin,
        WIFI_C6_DISABLED_LEVEL,
        &settle_ms);

    if (level_after_hold != WIFI_C6_DISABLED_LEVEL) {
        ESP_LOGE(
            TAG,
            "ESP32-C6 CHIP_PU still reads %d after %" PRIu32
            " ms; expected LOW",
            level_after_hold,
            settle_ms);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "ESP32-C6 disabled: GPIO%d CHIP_PU strongly held LOW "
        "(read-back=%d after %" PRIu32 " ms)",
        pin,
        level_after_hold,
        settle_ms);

    return ESP_OK;
}

esp_err_t component_wifi_restore_after_failed_sleep(void)
{
    /* C6 is intentionally kept disabled permanently. */
    ESP_LOGI(TAG, "ESP32-C6 remains disabled");
    return ESP_OK;
}
