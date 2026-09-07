#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "C6_POWER_COMPANION";

/*
 * Waveshare ESP32-P4-NANO schematic:
 *   P4 GPIO6 -- R52 (0 ohm) --> C6 GPIO2
 *
 * Protocol used by this firmware:
 *
 *   COLD BOOT with GPIO2 LOW
 *       -> real P4 Deep-sleep boundary
 *       -> C6 immediately calls esp_deep_sleep_start()
 *
 *   COLD BOOT with GPIO2 HIGH
 *       -> normal P4 application is bringing the C6 up
 *       -> C6 starts at 80 MHz
 *       -> P4 then drives GPIO2 LOW and the running C6 changes to 160 MHz
 *
 *   RUNNING transition LOW -> HIGH
 *       -> P4 is entering Light-sleep
 *       -> lower C6 CPU to 80 MHz WITHOUT reset/power-cycle
 *
 *   RUNNING transition HIGH -> LOW
 *       -> P4 woke to ACTIVE
 *       -> restore C6 CPU to 160 MHz WITHOUT reset/power-cycle
 */
#define C6_P4_MODE_GPIO               GPIO_NUM_2
#define C6_P4_MODE_ACTIVE_LEVEL       0
#define C6_P4_MODE_LIGHT_LEVEL        1
#define C6_LIGHT_CPU_MHZ              80
#define C6_ACTIVE_CPU_MHZ             160
#define C6_MODE_POLL_MS               20U
#define C6_BOOT_SAMPLE_SETTLE_MS      20U
#define C6_DEEP_SLEEP_LOG_DELAY_MS    100U

static esp_err_t configure_fixed_cpu_frequency(int mhz)
{
    esp_pm_config_t pm_config = {
        .max_freq_mhz = mhz,
        .min_freq_mhz = mhz,
        .light_sleep_enable = false,
    };

    const esp_err_t ret = esp_pm_configure(&pm_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_pm_configure(%d MHz) failed: %s",
                 mhz,
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGW(TAG,
             "CPU policy applied: min=%d MHz max=%d MHz auto_light_sleep=OFF",
             mhz,
             mhz);
    return ESP_OK;
}

static void enter_c6_self_deep_sleep(void)
{
    ESP_LOGW(
        TAG,
        "MODE: cold boot with P4 mode LOW -> C6 self-Deep-sleep");
    ESP_LOGW(
        TAG,
        "POLICY: no C6 wake source; external CHIP_PU/reset/power is required");

    vTaskDelay(pdMS_TO_TICKS(C6_DEEP_SLEEP_LOG_DELAY_MS));
    fflush(stdout);
    esp_deep_sleep_start();
    abort();
}

void app_main(void)
{
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    const esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();

    gpio_config_t mode_config = {
        .pin_bit_mask = 1ULL << C6_P4_MODE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&mode_config));

    vTaskDelay(pdMS_TO_TICKS(C6_BOOT_SAMPLE_SETTLE_MS));
    int mode_level = gpio_get_level(C6_P4_MODE_GPIO);

    ESP_LOGW(
        TAG,
        "BOOT: reset_reason=%d wake_cause=%d P4_MODE_GPIO2=%d",
        (int)reset_reason,
        (int)wake_cause,
        mode_level);

    if (mode_level != C6_P4_MODE_LIGHT_LEVEL) {
        enter_c6_self_deep_sleep();
    }

    ESP_ERROR_CHECK(configure_fixed_cpu_frequency(C6_LIGHT_CPU_MHZ));
    ESP_LOGW(
        TAG,
        "MODE: companion running at %d MHz; waiting for live P4 mode changes",
        C6_LIGHT_CPU_MHZ);

    int last_mode = mode_level;

    while (true) {
        mode_level = gpio_get_level(C6_P4_MODE_GPIO);

        if (mode_level != last_mode) {
            if (mode_level == C6_P4_MODE_LIGHT_LEVEL) {
                ESP_ERROR_CHECK(configure_fixed_cpu_frequency(C6_LIGHT_CPU_MHZ));
                ESP_LOGW(
                    TAG,
                    "MODE: P4 LIGHT_SLEEP -> %d MHz, no C6 reset",
                    C6_LIGHT_CPU_MHZ);
            } else {
                ESP_ERROR_CHECK(configure_fixed_cpu_frequency(C6_ACTIVE_CPU_MHZ));
                ESP_LOGW(
                    TAG,
                    "MODE: P4 ACTIVE -> %d MHz, no C6 reset",
                    C6_ACTIVE_CPU_MHZ);
            }

            last_mode = mode_level;
        }

        vTaskDelay(pdMS_TO_TICKS(C6_MODE_POLL_MS));
    }
}
