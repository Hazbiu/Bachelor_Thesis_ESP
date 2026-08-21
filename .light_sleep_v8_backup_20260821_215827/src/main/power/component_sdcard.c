#include "power_save/component_sdcard.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "bsp/esp-bsp.h"
#include "config/app_config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Waveshare ESP32-P4-NANO microSD power circuit:
 *
 * ESP32-P4 GPIO45 -> Q1 AO3401 gate -> SD1_VDD
 *
 * Q1 is a P-channel MOSFET:
 *     GPIO45 LOW  = microSD power ON
 *     GPIO45 HIGH = microSD power OFF
 */
#define SDCARD_POWER_GPIO       APP_PWR_SDCARD_POWER_GPIO
#define SDCARD_POWER_ON_LEVEL   APP_PWR_SDCARD_POWER_ON_LEVEL
#define SDCARD_POWER_OFF_LEVEL  APP_PWR_SDCARD_POWER_OFF_LEVEL
#define SDCARD_POWER_SETTLE_MS  APP_PWR_SDCARD_POWER_SETTLE_MS

static const char *TAG = "component_sdcard";

static bool s_was_mounted;
static bool s_power_disabled;

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

static esp_err_t configure_sdcard_power_gpio(void)
{
    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << SDCARD_POWER_GPIO,
        /* INPUT_OUTPUT so the pre-sleep rail audit can read the pad back. */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&io_config);
}

static esp_err_t remount_after_shutdown_error(void)
{
    if (!s_was_mounted) {
        return ESP_OK;
    }

    vTaskDelay(pdMS_TO_TICKS(SDCARD_POWER_SETTLE_MS));

    esp_err_t ret = bsp_sdcard_mount();
    if (ret == ESP_OK) {
        ESP_LOGW(TAG, "microSD remounted after shutdown error");
    } else {
        ESP_LOGE(
            TAG,
            "Could not remount microSD after shutdown error: %s",
            esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t component_sdcard_disable_for_deep_sleep(void)
{
    s_was_mounted = (bsp_sdcard != NULL);
    s_power_disabled = false;

    /*
     * Never remove card power while FATFS is still mounted. The Waveshare BSP
     * unmount call flushes and unregisters the /sdcard filesystem and
     * deinitializes the SDMMC host.
     */
    if (s_was_mounted) {
        esp_err_t unmount_ret = bsp_sdcard_unmount();

        if (unmount_ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "microSD unmount failed; power remains ON: %s",
                esp_err_to_name(unmount_ret));
            return unmount_ret;
        }

        /*
         * The current Waveshare BSP does not clear its global card pointer
         * after a successful unmount, so clear it here to avoid a stale
         * pointer and to make later state checks reliable.
         */
        bsp_sdcard = NULL;

        ESP_LOGI(TAG, "microSD filesystem unmounted successfully");
    } else {
        ESP_LOGI(TAG, "microSD was not mounted; skipping filesystem unmount");
    }

    esp_err_t ret = configure_sdcard_power_gpio();
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "GPIO%d configuration failed: %s",
            SDCARD_POWER_GPIO,
            esp_err_to_name(ret));

        (void)remount_after_shutdown_error();
        return ret;
    }

    /*
     * HIGH switches Q1 off and disconnects SD1_VDD from the card.
     */
    ret = gpio_set_level(
        SDCARD_POWER_GPIO,
        SDCARD_POWER_OFF_LEVEL);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not switch microSD power OFF: %s",
            esp_err_to_name(ret));

        (void)gpio_set_level(
            SDCARD_POWER_GPIO,
            SDCARD_POWER_ON_LEVEL);

        (void)remount_after_shutdown_error();
        return ret;
    }

    /*
     * The board fits a pull-down on this gate so the card powers up by default.
     * Stop ESP-IDF from switching the pad to its sleep configuration, otherwise
     * that pull-down can win at the sleep boundary and re-power the card.
     */
    ret = gpio_sleep_sel_dis(SDCARD_POWER_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not disable sleep switching for GPIO%d: %s",
            SDCARD_POWER_GPIO,
            esp_err_to_name(ret));

        (void)gpio_set_level(
            SDCARD_POWER_GPIO,
            SDCARD_POWER_ON_LEVEL);

        (void)remount_after_shutdown_error();
        return ret;
    }

    /*
     * Retain GPIO45 HIGH while the normal GPIO/IOMUX domain is powered down.
     * ESP32-P4 rev < 3.0 does not retain the hold after waking, which is useful
     * here because the board's pull-down then enables the card for the reboot.
     */
    ret = gpio_hold_en(SDCARD_POWER_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not hold GPIO%d HIGH: %s",
            SDCARD_POWER_GPIO,
            esp_err_to_name(ret));

        (void)gpio_set_level(
            SDCARD_POWER_GPIO,
            SDCARD_POWER_ON_LEVEL);

        (void)remount_after_shutdown_error();
        return ret;
    }

    s_power_disabled = true;

    uint32_t settle_ms = 0;
    const int level_after_hold = wait_for_pad_level(
        SDCARD_POWER_GPIO,
        SDCARD_POWER_OFF_LEVEL,
        &settle_ms);

    if (level_after_hold != SDCARD_POWER_OFF_LEVEL) {
        ESP_LOGE(
            TAG,
            "microSD gate still reads %d after holding for %" PRIu32
            " ms (expected %d); SD1_VDD is still connected",
            level_after_hold,
            settle_ms,
            SDCARD_POWER_OFF_LEVEL);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "microSD power disabled: GPIO%d held HIGH "
        "(read-back=%d after %" PRIu32 " ms)",
        SDCARD_POWER_GPIO,
        level_after_hold,
        settle_ms);

    return ESP_OK;
}

esp_err_t component_sdcard_restore_after_failed_sleep(void)
{
    esp_err_t first_error = ESP_OK;

    /*
     * This function is normally unreachable because esp_deep_sleep_start()
     * does not return after successful entry.
     */
    if (s_power_disabled) {
        esp_err_t ret = gpio_hold_dis(SDCARD_POWER_GPIO);
        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Could not release GPIO%d hold: %s",
                SDCARD_POWER_GPIO,
                esp_err_to_name(ret));
            first_error = ret;
        }

        ret = gpio_set_level(
            SDCARD_POWER_GPIO,
            SDCARD_POWER_ON_LEVEL);

        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Could not restore microSD power: %s",
                esp_err_to_name(ret));

            if (first_error == ESP_OK) {
                first_error = ret;
            }
        } else {
            ESP_LOGW(TAG, "Deep sleep failed; microSD power restored");
        }

        s_power_disabled = false;
        vTaskDelay(pdMS_TO_TICKS(SDCARD_POWER_SETTLE_MS));
    }

    if (s_was_mounted && bsp_sdcard == NULL) {
        esp_err_t mount_ret = bsp_sdcard_mount();

        if (mount_ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Could not remount microSD after failed sleep: %s",
                esp_err_to_name(mount_ret));

            if (first_error == ESP_OK) {
                first_error = mount_ret;
            }
        } else {
            ESP_LOGW(TAG, "microSD remounted after failed sleep");
        }
    }

    s_was_mounted = false;
    return first_error;
}
