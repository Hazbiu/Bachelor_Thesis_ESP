#include "diagnostics/sleep_power_profile.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "config/app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool s_active;
static unsigned s_stage;
static const char *TAG = "POWER_PROFILE";

void sleep_power_profile_start(const char *origin)
{
    s_active = APP_SLEEP_POWER_PROFILE_STAGE_DELAY_MS > 0;
    s_stage = 0;
    if (!s_active) {
        return;
    }

    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG,
             "event=PROFILE_START origin=\"%s\" pause_ms=%u "
             "target=TRUE_P4_DEEP_SLEEP",
             origin, (unsigned)APP_SLEEP_POWER_PROFILE_STAGE_DELAY_MS);
    ESP_LOGW(TAG,
             "Measurement windows use vTaskDelay: P4 is awake, other tasks may "
             "run. Read current on the external meter during MEASURE. Compare "
             "adjacent windows; these are not final sleep-current readings.");
}

void sleep_power_profile_stop(void)
{
    s_active = false;
}

void sleep_power_profile_before(const char *component)
{
    if (s_active) {
        ESP_LOGI(TAG, "stage=%02u event=BEGIN component=\"%s\"",
                 s_stage + 1U, component);
    }
}

void sleep_power_profile_quiet_wait(void)
{
    if (!s_active) {
        return;
    }

    /* Use a monotonic deadline, so tick rounding never shortens the requested
     * three-second interval. Yield to FreeRTOS; never spin or mask interrupts.
     * This also remains safe after UART detach: there are no log calls here. */
    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)APP_SLEEP_POWER_PROFILE_STAGE_DELAY_MS * 1000LL;
    for (;;) {
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            break;
        }
        const uint32_t remaining_ms =
            (uint32_t)((remaining_us + 999LL) / 1000LL);
        const TickType_t ticks = pdMS_TO_TICKS(remaining_ms);
        vTaskDelay(ticks > 0 ? ticks : 1);
    }
}

void sleep_power_profile_after(const char *component, const char *result)
{
    if (!s_active) {
        return;
    }

    ++s_stage;
    ESP_LOGI(TAG,
             "stage=%02u event=MEASURE component=\"%s\" result=%s "
             "pause_ms=%u p4=AWAKE",
             s_stage, component, result,
             (unsigned)APP_SLEEP_POWER_PROFILE_STAGE_DELAY_MS);
    fflush(stdout);
    const int64_t start_us = esp_timer_get_time();
    sleep_power_profile_quiet_wait();
    ESP_LOGI(TAG,
             "stage=%02u event=READY component=\"%s\" elapsed_ms=%" PRId64,
             s_stage, component,
             (int64_t)((esp_timer_get_time() - start_us) / 1000LL));
}

void sleep_power_profile_final_boundary(bool detach_uart)
{
    if (s_active) {
        ++s_stage;
        ESP_LOGW(TAG,
                 "stage=%02u event=FINAL_BOUNDARY_NEXT component=\"%s\" "
                 "quiet_pause_ms=%u then=esp_deep_sleep_start "
                 "application_logs_after_boundary=OFF",
                 s_stage, detach_uart ? "UART0 GPIO37/38 detach" : "P4 sleep entry",
                 (unsigned)APP_SLEEP_POWER_PROFILE_STAGE_DELAY_MS);
        ESP_LOGW(TAG,
                 "After the quiet pause, observe the final meter plateau. "
                 "P4 internal domains switch at actual sleep entry, not when "
                 "their OFF policy is configured. GPIO levels logged before "
                 "entry do not prove pad retention in Deep-sleep.");
    }
}
