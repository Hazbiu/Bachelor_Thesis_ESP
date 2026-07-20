#include "power_save/deep_sleep.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "bsp/esp-bsp.h"

/*
 * GT9271 INT is normally pulled HIGH and goes LOW when a touch is available.
 *
 * This must be the ESP32-P4 GPIO connected to the display's GT9271 INT pin.
 * Do not use a GPIO number until you have confirmed the display wiring.
 */
#define TOUCH_INT_GPIO GPIO_NUM_XX

static const char *TAG = "deep_sleep";

void enter_deep_sleep(void)
{
    const uint64_t touch_pin_mask = 1ULL << TOUCH_INT_GPIO;

    gpio_config_t touch_int_config = {
        .pin_bit_mask = touch_pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&touch_int_config));

    /*
     * Do not sleep while INT is already LOW, otherwise the ESP32-P4 wakes
     * immediately. Read/clear the pending GT9271 touch event first.
     */
    if (gpio_get_level(TOUCH_INT_GPIO) == 0) {
        ESP_LOGW(TAG, "Touch INT is already LOW; deep sleep cancelled");
        return;
    }

    /*
     * ESP32-P4 deep-sleep GPIO wakeup.
     * Touching the display drives GT9271 INT LOW.
     */
    ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup(
        touch_pin_mask,
        ESP_GPIO_WAKEUP_GPIO_LOW));

    /* Switch off the visible display, but keep GT9271 power enabled. */
    bsp_display_backlight_off();

    ESP_LOGI(TAG, "Entering deep sleep; wake on GT9271 INT GPIO %d",
             TOUCH_INT_GPIO);

    esp_deep_sleep_start();
}