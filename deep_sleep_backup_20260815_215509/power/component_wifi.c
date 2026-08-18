#include "power_save/component_wifi.h"

#include <stdbool.h>

#include "config/app_config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

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

esp_err_t component_wifi_disable_for_deep_sleep(void)
{
    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << WIFI_C6_CHIP_PU_GPIO,
        /*
         * INPUT_OUTPUT instead of OUTPUT so the pad can be read back after the
         * hold is applied. The pre-sleep rail audit in deep_sleep.c relies on
         * this to prove that CHIP_PU really stays LOW; a plain OUTPUT pad has
         * its input path disabled and always reads 0.
         */
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
            WIFI_C6_CHIP_PU_GPIO,
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * LOW holds the external ESP32-C6 in reset.
     */
    ret = gpio_set_level(WIFI_C6_CHIP_PU_GPIO, WIFI_C6_DISABLED_LEVEL);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not drive ESP32-C6 CHIP_PU LOW: %s",
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * ESP-IDF normally switches GPIO54 to its sleep configuration.
     * The board's external pull-up then raises CHIP_PU to 3.3 V and
     * starts the ESP32-C6 again.
     *
     * Disable sleep-configuration switching for GPIO54 before holding
     * the pin, so its active OUTPUT-LOW configuration is retained.
     */
    ret = gpio_sleep_sel_dis(WIFI_C6_CHIP_PU_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not disable sleep switching for GPIO%d: %s",
            WIFI_C6_CHIP_PU_GPIO,
            esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_hold_en(WIFI_C6_CHIP_PU_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not hold GPIO%d LOW: %s",
            WIFI_C6_CHIP_PU_GPIO,
            esp_err_to_name(ret));
        return ret;
    }

    s_c6_disabled = true;

    /*
     * Read the pad back through the still-enabled input path. If this reports
     * HIGH, the external pull-up has won and the ESP32-C6 is running, which is
     * worth tens of milliamps during "Deep-sleep".
     */
    const int level_after_hold = gpio_get_level(WIFI_C6_CHIP_PU_GPIO);

    if (level_after_hold != WIFI_C6_DISABLED_LEVEL) {
        ESP_LOGE(
            TAG,
            "ESP32-C6 CHIP_PU reads %d after hold (expected %d); the "
            "coprocessor is still powered",
            level_after_hold,
            WIFI_C6_DISABLED_LEVEL);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "ESP32-C6 disabled: GPIO%d CHIP_PU held LOW (read-back=%d)",
        WIFI_C6_CHIP_PU_GPIO,
        level_after_hold);

    return ESP_OK;
}

esp_err_t component_wifi_restore_after_failed_sleep(void)
{
    /*
     * This function is reached only if esp_deep_sleep_start()
     * unexpectedly returns.
     */
    if (!s_c6_disabled) {
        return ESP_OK;
    }

    esp_err_t ret = gpio_hold_dis(WIFI_C6_CHIP_PU_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not release GPIO%d hold: %s",
            WIFI_C6_CHIP_PU_GPIO,
            esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_set_level(WIFI_C6_CHIP_PU_GPIO, WIFI_C6_ENABLED_LEVEL);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not drive ESP32-C6 CHIP_PU HIGH: %s",
            esp_err_to_name(ret));
        return ret;
    }

    s_c6_disabled = false;

    ESP_LOGW(TAG, "Deep sleep failed; ESP32-C6 re-enabled");
    return ESP_OK;
}
