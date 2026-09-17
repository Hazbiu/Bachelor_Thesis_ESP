
#include "app/app_runtime.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>

#include "app/camera/camera_session.h"
#include "app/controller/app_controller.h"
#include "app/navigation/app_navigation.h"
#include "config/app_config.h"
#include "config/log_config.h"
#include "diagnostics/app_logging.h"
#include "diagnostics/core_trace.h"
#include "diagnostics/cpu_stats.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "domain/ports/system_adapters_port.h"
#include "services/power/sleep/wake_up.h"
#include "app/configuration/app_configuration.h"


static const char *TAG = "app_main";


void app_runtime_start(void)
{
    app_logging_init();

    app_controller_init(NULL, NULL);

    /*
     * esp_timer restarts after real Deep-sleep. Capture the earliest application
     * timestamp so the GPIO3 wake metric measures app-runtime entry through the
     * first successfully presented camera frame. ROM/bootloader latency is not
     * part of this software-only number.
     */
    const int64_t app_runtime_entry_us = esp_timer_get_time();

    /*
     * Light-sleep resumes in place and never comes through this boot path.
     *
     * Real Deep-sleep wake restarts the ESP32-P4 from app_main(), so remember
     * whether this boot was caused by the GPIO3 LCD/power button.
     */
    const esp_sleep_wakeup_cause_t wake_cause =
        esp_sleep_get_wakeup_cause();

    const uint64_t wake_gpio_mask =
        wake_cause == ESP_SLEEP_WAKEUP_GPIO
            ? esp_sleep_get_gpio_wakeup_status()
            : 0ULL;

    const bool woke_from_lcd_button =
        wake_cause == ESP_SLEEP_WAKEUP_GPIO &&
        (wake_gpio_mask &
         (1ULL << APP_DEEP_SLEEP_BUTTON_GPIO)) != 0ULL;

    esp_err_t settings_ret = app_configuration_init();
    if (settings_ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Persistent settings unavailable; in-memory defaults are active: %s",
                 esp_err_to_name(settings_ret));
    }

    esp_err_t power_policy_ret = app_configuration_apply_saved_policy();
    if (power_policy_ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Initial power policy application failed: %s",
                 esp_err_to_name(power_policy_ret));
    }

    ESP_LOGI(TAG,
             "[CORE-PROOF] Application split: system_cpu=%d ai_cpu=%d",
             APP_SYSTEM_WORKER_CORE,
             APP_AI_WORKER_CORE);

    report_wake_reason();
    core_trace(TAG, "APP_MAIN_START");

    esp_err_t cpu_power_ret = system_cpu_power_init();
    if (cpu_power_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "CPU power management initialization failed: %s",
            esp_err_to_name(cpu_power_ret));
    }

#if APP_DIAGNOSTICS_CPU_STATS_ENABLED
    diagnostics_start_cpu_stats_monitor();
#endif

    if (woke_from_lcd_button) {
        /*
         * Deep-sleep wake path:
         *   - rebuild only the display/touch infrastructure required by camera;
         *   - never create/show launcher widgets;
         *   - keep the panel dark until the first live camera frame;
         *   - retain the existing logical BOOTING->LAUNCHER->CAMERA_ACTIVE FSM
         *     transitions so the state machine structure stays unchanged.
         */
        ESP_LOGI(
            TAG,
            "[WAKE-TIME] mode=DEEP_SLEEP event=APP_RUNTIME_ENTRY "
            "timestamp_us=%" PRId64
            " scope=EXCLUDES_ROM_BOOTLOADER",
            app_runtime_entry_us);

        if (camera_session_prepare_direct_wake_display() != ESP_OK) {
            ESP_LOGE(TAG, "[DEEP-WAKE] could not prepare direct camera display");
            return;
        }

        if (app_controller_handle_event(APP_EVENT_BOOT_COMPLETE)) {
            ESP_LOGI(
                TAG,
                "[APP-STATE] BOOTING -> LAUNCHER "
                "(logical state only; launcher UI bypassed)");
        } else {
            ESP_LOGW(
                TAG,
                "[DEEP-WAKE] BOOT_COMPLETE did not cause logical launcher transition");
        }

        (void)camera_session_setup_power_management();

        ESP_LOGI(
            TAG,
            "GPIO%d Deep-sleep wake detected; bypassing launcher UI and "
            "starting camera directly",
            (int)APP_DEEP_SLEEP_BUTTON_GPIO);

        camera_session_start_direct_after_deep_sleep(app_runtime_entry_us);
        return;
    }

    /*
     * Normal cold boot is unchanged: create and show the launcher, then wait for
     * the user's Start action.
     */
    if (app_navigation_start() != ESP_OK) {
        return;
    }

    (void)camera_session_setup_power_management();
}
