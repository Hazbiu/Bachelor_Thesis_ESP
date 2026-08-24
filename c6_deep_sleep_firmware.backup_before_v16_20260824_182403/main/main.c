#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "C6_SELF_SLEEP";

/*
 * ESP32-C6 self-Deep-sleep diagnostic firmware.
 *
 * No wake source is configured. After boot the C6 waits 250 ms so the UART
 * message is visible, then enters Deep-sleep indefinitely. It wakes only after
 * an external reset/power cycle/CHIP_PU reset.
 */
void app_main(void)
{
    ESP_LOGW(TAG,
             "BOOT: self-sleep firmware running reset_reason=%d wake_cause=%d",
             (int)esp_reset_reason(),
             (int)esp_sleep_get_wakeup_cause());

    ESP_LOGW(TAG,
             "POLICY: no wake source; C6 will remain in Deep-sleep until external reset");

    vTaskDelay(pdMS_TO_TICKS(250));

    ESP_LOGW(TAG, "ENTER: calling esp_deep_sleep_start() now");
    fflush(stdout);

    /* No esp_sleep_enable_*_wakeup() call on purpose. */
    esp_deep_sleep_start();

    abort();
}
