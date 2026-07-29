#include "power_save/component_wifi.h"

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
#define WIFI_C6_CHIP_PU_GPIO GPIO_NUM_54

static const char *TAG = "component_wifi";

esp_err_t component_wifi_disable_for_deep_sleep(void)
{
    /*
     * Do not name this variable gpio_config. That would hide the
     * gpio_config() function and cause a compiler error.
     */
    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << WIFI_C6_CHIP_PU_GPIO,
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
            WIFI_C6_CHIP_PU_GPIO,
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * Hold the ESP32-C6 in reset.
     */
    ret = gpio_set_level(WIFI_C6_CHIP_PU_GPIO, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not drive ESP32-C6 CHIP_PU LOW: %s",
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * ESP32-P4 supports holding an individual output GPIO while its
     * GPIO/IOMUX power domain is switched off. ESP-IDF for ESP32-P4
     * does not provide gpio_deep_sleep_hold_en().
     */
    ret = gpio_hold_en(WIFI_C6_CHIP_PU_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not hold GPIO%d LOW: %s",
            WIFI_C6_CHIP_PU_GPIO,
            esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(
        TAG,
        "ESP32-C6 disabled: GPIO%d CHIP_PU held LOW",
        WIFI_C6_CHIP_PU_GPIO);

    return ESP_OK;
}

esp_err_t component_wifi_restore_after_failed_sleep(void)
{
    /*
     * This function is reached only if esp_deep_sleep_start()
     * unexpectedly returns.
     */
    esp_err_t ret = gpio_hold_dis(WIFI_C6_CHIP_PU_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not release GPIO%d hold: %s",
            WIFI_C6_CHIP_PU_GPIO,
            esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_set_level(WIFI_C6_CHIP_PU_GPIO, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not drive ESP32-C6 CHIP_PU HIGH: %s",
            esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGW(TAG, "Deep sleep failed; ESP32-C6 re-enabled");
    return ESP_OK;
}
