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
 * No wake source is configured. Once this firmware boots it enters the C6's
 * own Deep-sleep and remains there until CHIP_PU/reset/power is toggled.
 *
 * This is intentional for the ESP32-P4-NANO power experiment:
 * - Normal P4 application keeps C6 CHIP_PU LOW while P4 is active.
 * - On the tested P4 v1.3 board, GPIO54 rises when the P4 reaches real
 *   Deep-sleep.
 * - That allows the C6 to boot.
 * - The C6 then puts itself into its own Deep-sleep instead of remaining
 *   awake and consuming tens of mA.
 */

void app_main(void)
{
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    const esp_sleep_wakeup_cause_t wake_cause =
        esp_sleep_get_wakeup_cause();

    ESP_LOGW(
        TAG,
        "BOOT: C6 self-Deep-sleep firmware running "
        "(reset_reason=%d wake_cause=%d)",
        (int)reset_reason,
        (int)wake_cause);

    ESP_LOGW(
        TAG,
        "POLICY: no C6 wake source; external reset/CHIP_PU is required");

    /*
     * Keep only a short boot window. This is long enough for a UART diagnostic
     * if the temporary wires are still attached but short enough not to leave
     * the C6 awake during the P4 power test.
     */
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGW(TAG, "ENTER: C6 -> esp_deep_sleep_start()");
    fflush(stdout);

    esp_deep_sleep_start();

    abort();
}
