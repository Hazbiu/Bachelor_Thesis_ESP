
#include "app/app_runtime.h"

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
#include "domain/ports/system_adapters_port.h"
#include "services/power/sleep/wake_up.h"
#include "app/configuration/app_configuration.h"


static const char *TAG = "app_main";


void app_runtime_start(void)
{
    app_logging_init();

    app_controller_init(NULL, NULL);

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

    if (app_navigation_start() != ESP_OK) {
        return;
    }

    (void)camera_session_setup_power_management();
}
