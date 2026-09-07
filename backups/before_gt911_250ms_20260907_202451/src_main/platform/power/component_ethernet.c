#include "platform/power/component_ethernet.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "config/app_config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ETHERNET_PHY_RESET_GPIO     APP_PWR_ETHERNET_PHY_RESET_GPIO
#define ETHERNET_PHY_RESET_ACTIVE   APP_PWR_ETHERNET_RESET_ACTIVE_LEVEL
#define ETHERNET_PHY_RESET_RELEASED APP_PWR_ETHERNET_RESET_RELEASED_LEVEL

static const char *TAG = "component_ethernet";

static bool s_reset_asserted;
static bool s_user_enabled = true;


static int wait_for_pad_level(
    gpio_num_t gpio_num,
    int expected_level,
    uint32_t *settle_ms_out)
{
    uint32_t waited_ms = 0;
    int level = gpio_get_level(gpio_num);

    while (level != expected_level &&
           waited_ms < APP_PWR_HOLD_VERIFY_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(APP_PWR_HOLD_VERIFY_POLL_MS));
        waited_ms += APP_PWR_HOLD_VERIFY_POLL_MS;
        level = gpio_get_level(gpio_num);
    }

    if (settle_ms_out != NULL) {
        *settle_ms_out = waited_ms;
    }

    return level;
}


static esp_err_t configure_phy_reset_level(
    int level,
    bool hold,
    bool wait_for_settle)
{
    esp_err_t ret = gpio_hold_dis(ETHERNET_PHY_RESET_GPIO);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(
            TAG,
            "Could not release previous GPIO%d hold: %s",
            ETHERNET_PHY_RESET_GPIO,
            esp_err_to_name(ret));
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << ETHERNET_PHY_RESET_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_level(ETHERNET_PHY_RESET_GPIO, level);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_sleep_sel_dis(ETHERNET_PHY_RESET_GPIO);
    if (ret != ESP_OK) {
        return ret;
    }

    if (hold) {
        ret = gpio_hold_en(ETHERNET_PHY_RESET_GPIO);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    uint32_t settle_ms = 0;
    const int readback = wait_for_settle
        ? wait_for_pad_level(
              ETHERNET_PHY_RESET_GPIO,
              level,
              &settle_ms)
        : gpio_get_level(ETHERNET_PHY_RESET_GPIO);

    if (readback != level) {
        ESP_LOGE(
            TAG,
            "IP101GRI RESET read-back=%d expected=%d after %" PRIu32 " ms",
            readback,
            level,
            settle_ms);
        return ESP_ERR_INVALID_STATE;
    }

    s_reset_asserted = (level == ETHERNET_PHY_RESET_ACTIVE);

    ESP_LOGI(
        TAG,
        "IP101GRI RESET GPIO%d=%d hold=%s readback=%d settle_ms=%" PRIu32,
        ETHERNET_PHY_RESET_GPIO,
        level,
        hold ? "ON" : "OFF",
        readback,
        settle_ms);

    return ESP_OK;
}


esp_err_t component_ethernet_hold_reset_for_light_sleep(void)
{
    const esp_err_t ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_ACTIVE,
        true,
        true);

    if (ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "IP101GRI Light-sleep state: RESET GPIO%d LOW and held",
            ETHERNET_PHY_RESET_GPIO);
    }

    return ret;
}


esp_err_t component_ethernet_restore_after_light_sleep(void)
{
    if (!s_user_enabled) {
        return configure_phy_reset_level(
            ETHERNET_PHY_RESET_ACTIVE,
            true,
            true);
    }

    const esp_err_t ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_RELEASED,
        false,
        true);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "IP101GRI reset released after Light-sleep");
    }

    return ret;
}


esp_err_t component_ethernet_disable_for_deep_sleep(void)
{
    /*
     * Preserve the exact lower-current Light-sleep electrical state.
     * Do not release RESET and wake the PHY merely to program BMCR.
     */
    const esp_err_t ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_ACTIVE,
        true,
        true);

    if (ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "IP101GRI Deep-sleep state: hardware RESET GPIO%d LOW and held; "
            "BMCR wake/reprogram step intentionally skipped",
            ETHERNET_PHY_RESET_GPIO);
    }

    return ret;
}


esp_err_t component_ethernet_verify_power_down(void)
{
    const int level = gpio_get_level(ETHERNET_PHY_RESET_GPIO);

    if (level != ETHERNET_PHY_RESET_ACTIVE) {
        ESP_LOGE(
            TAG,
            "IP101GRI Deep-sleep verification failed: RESET GPIO%d=%d "
            "expected=%d",
            ETHERNET_PHY_RESET_GPIO,
            level,
            ETHERNET_PHY_RESET_ACTIVE);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "IP101GRI final audit OK: hardware RESET GPIO%d LOW",
        ETHERNET_PHY_RESET_GPIO);

    return ESP_OK;
}


esp_err_t component_ethernet_restore_after_failed_sleep(void)
{
    if (!s_user_enabled) {
        ESP_LOGI(
            TAG,
            "IP101GRI remains in hardware RESET because saved Ethernet policy is OFF");
        return configure_phy_reset_level(
            ETHERNET_PHY_RESET_ACTIVE,
            true,
            true);
    }

    const esp_err_t ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_RELEASED,
        false,
        true);

    if (ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "IP101GRI hardware RESET released after failed Deep-sleep entry");
    }

    return ret;
}


esp_err_t component_ethernet_set_enabled(bool enabled)
{
    const bool previous_policy = s_user_enabled;
    s_user_enabled = enabled;

    const esp_err_t ret = configure_phy_reset_level(
        enabled ? ETHERNET_PHY_RESET_RELEASED : ETHERNET_PHY_RESET_ACTIVE,
        !enabled,
        true);

    if (ret != ESP_OK) {
        s_user_enabled = previous_policy;
        return ret;
    }

    ESP_LOGI(
        TAG,
        "IP101GRI saved policy applied: %s",
        enabled ? "ENABLED (RESET released)" : "DISABLED (RESET LOW held)");

    return ESP_OK;
}


bool component_ethernet_is_enabled(void)
{
    return s_user_enabled && !s_reset_asserted;
}
