#include "services/power/sleep/wake_up.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "wake_up";

void report_wake_reason(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    switch (cause) {
    case ESP_SLEEP_WAKEUP_GPIO: {
        uint64_t wake_gpio_mask = esp_sleep_get_gpio_wakeup_status();

        ESP_LOGI(
            TAG,
            "Woken from deep sleep by GPIO, mask=0x%llx",
            (unsigned long long)wake_gpio_mask
        );

        if (wake_gpio_mask & (1ULL << GPIO_NUM_3)) {
            ESP_LOGI(TAG, "GPIO3 rocker transition woke the system");
        }

        break;
    }

    case ESP_SLEEP_WAKEUP_TIMER:
        ESP_LOGI(TAG, "Woken from deep sleep by timer");
        break;

    case ESP_SLEEP_WAKEUP_UNDEFINED:
        ESP_LOGI(TAG, "Normal power-on or reset (not deep-sleep wake)");
        break;

    default:
        ESP_LOGI(TAG, "Wake cause: %d", cause);
        break;
    }
}
