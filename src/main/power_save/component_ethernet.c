#include "power_save/component_ethernet.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#define ETHERNET_PHY_RESET_GPIO GPIO_NUM_51
#define ETHERNET_PHY_RESET_ACTIVE 0
#define ETHERNET_PHY_RESET_RELEASED 1

static const char *TAG = "component_ethernet";

esp_err_t component_ethernet_disable_for_deep_sleep(void)
{
    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << ETHERNET_PHY_RESET_GPIO,
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
            ETHERNET_PHY_RESET_GPIO,
            esp_err_to_name(ret));

        return ret;
    }

    ret = gpio_set_level(
        ETHERNET_PHY_RESET_GPIO,
        ETHERNET_PHY_RESET_ACTIVE);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not drive Ethernet PHY RESET LOW: %s",
            esp_err_to_name(ret));

        return ret;
    }

    ret = gpio_hold_en(ETHERNET_PHY_RESET_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not hold GPIO%d LOW: %s",
            ETHERNET_PHY_RESET_GPIO,
            esp_err_to_name(ret));

        return ret;
    }

    ESP_LOGI(
        TAG,
        "IP101GRI Ethernet PHY held in reset: GPIO%d LOW",
        ETHERNET_PHY_RESET_GPIO);

    return ESP_OK;
}

esp_err_t component_ethernet_restore_after_failed_sleep(void)
{
    esp_err_t ret = gpio_hold_dis(ETHERNET_PHY_RESET_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not release GPIO%d hold: %s",
            ETHERNET_PHY_RESET_GPIO,
            esp_err_to_name(ret));

        return ret;
    }

    ret = gpio_set_level(
        ETHERNET_PHY_RESET_GPIO,
        ETHERNET_PHY_RESET_RELEASED);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not release Ethernet PHY reset: %s",
            esp_err_to_name(ret));

        return ret;
    }

    ESP_LOGW(
        TAG,
        "Deep sleep failed; IP101GRI Ethernet PHY released");

    return ESP_OK;
}
