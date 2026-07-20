#include "power_save/wake_up.h"

#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "wake_up";

void report_wake_reason(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    switch (cause) {
    case ESP_SLEEP_WAKEUP_GPIO:
        ESP_LOGI(TAG, "Woken from deep sleep by GT9271 touch interrupt");
        break;

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