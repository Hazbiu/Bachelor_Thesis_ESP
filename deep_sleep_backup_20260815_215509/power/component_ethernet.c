#include "power_save/component_ethernet.h"

#include <stdbool.h>

#include "config/app_config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#define ETHERNET_PHY_RESET_GPIO     APP_PWR_ETHERNET_PHY_RESET_GPIO
#define ETHERNET_PHY_RESET_ACTIVE   APP_PWR_ETHERNET_RESET_ACTIVE_LEVEL
#define ETHERNET_PHY_RESET_RELEASED APP_PWR_ETHERNET_RESET_RELEASED_LEVEL

static const char *TAG = "component_ethernet";

static bool s_phy_held_in_reset;

esp_err_t component_ethernet_disable_for_deep_sleep(void)
{
    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << ETHERNET_PHY_RESET_GPIO,
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

    /*
     * The IP101GRI RESET line is externally pulled up. Without this call
     * ESP-IDF may swap the pad to its sleep configuration, the pull-up wins,
     * the PHY leaves reset and starts auto-negotiating at 20-40 mA. The same
     * fix was already necessary for the ESP32-C6 CHIP_PU pin.
     */
    ret = gpio_sleep_sel_dis(ETHERNET_PHY_RESET_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not disable sleep switching for GPIO%d: %s",
            ETHERNET_PHY_RESET_GPIO,
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

    s_phy_held_in_reset = true;

    const int level_after_hold = gpio_get_level(ETHERNET_PHY_RESET_GPIO);

    if (level_after_hold != ETHERNET_PHY_RESET_ACTIVE) {
        ESP_LOGE(
            TAG,
            "PHY RESET reads %d after hold (expected %d); the IP101GRI is "
            "still running",
            level_after_hold,
            ETHERNET_PHY_RESET_ACTIVE);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "IP101GRI Ethernet PHY held in reset: GPIO%d LOW (read-back=%d)",
        ETHERNET_PHY_RESET_GPIO,
        level_after_hold);

    return ESP_OK;
}

esp_err_t component_ethernet_restore_after_failed_sleep(void)
{
    if (!s_phy_held_in_reset) {
        return ESP_OK;
    }

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

    s_phy_held_in_reset = false;

    ESP_LOGW(
        TAG,
        "Deep sleep failed; IP101GRI Ethernet PHY released");

    return ESP_OK;
}
