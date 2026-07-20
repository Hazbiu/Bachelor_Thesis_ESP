#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "deep_sleep";

#define DEEP_SLEEP_WAKEUP_SECONDS 10ULL

void enter_deep_sleep(void)
{
    ESP_LOGI(
        TAG,
        "Touch interrupt is unavailable. "
        "Using a 10-second timer wakeup."
    );

    ESP_ERROR_CHECK(
        esp_sleep_enable_timer_wakeup(
            DEEP_SLEEP_WAKEUP_SECONDS * 1000000ULL
        )
    );

    ESP_LOGI(TAG, "Entering deep sleep");

    esp_deep_sleep_start();
}