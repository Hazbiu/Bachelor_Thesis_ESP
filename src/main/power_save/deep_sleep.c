#include "power_save/deep_sleep.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"

#include <stdio.h>

static const char *TAG = "deep_sleep";

#define WAKE_BUTTON_GPIO GPIO_NUM_3

void enter_deep_sleep(void)
{
    /*
     * Button wiring:
     *
     * GPIO3 ---- button ---- GND
     *
     * Released: GPIO3 = HIGH
     * Pressed:  GPIO3 = LOW
     */
    gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << WAKE_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&button_config));

    /*
     * Verify that GPIO3 is supported as a deep-sleep wake pin.
     */
    if (!esp_sleep_is_valid_wakeup_gpio(WAKE_BUTTON_GPIO)) {
        ESP_LOGE(
            TAG,
            "GPIO%d cannot wake this ESP32-P4 from deep sleep",
            WAKE_BUTTON_GPIO
        );

        return;
    }

    /*
     * Prevent immediate wake-up if the button is currently pressed.
     */
    if (gpio_get_level(WAKE_BUTTON_GPIO) == 0) {
        ESP_LOGW(
            TAG,
            "Button is currently pressed. Release it before deep sleep."
        );

        while (gpio_get_level(WAKE_BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /*
     * Wake when GPIO3 becomes LOW.
     *
     * No timer wake-up is enabled.
     */
    ESP_ERROR_CHECK(
        esp_deep_sleep_enable_gpio_wakeup(
            1ULL << WAKE_BUTTON_GPIO,
            ESP_GPIO_WAKEUP_GPIO_LOW
        )
    );

    ESP_LOGI(
        TAG,
        "Entering deep sleep. Press the GPIO%d button to wake up.",
        WAKE_BUTTON_GPIO
    );

    /*
     * Flush buffered output before entering deep sleep.
     */
    fflush(stdout);

    esp_deep_sleep_start();
}
