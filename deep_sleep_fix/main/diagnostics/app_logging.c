#include "diagnostics/app_logging.h"

#include "esp_log.h"

#include "config/log_config.h"

#define ENABLE_INFO(tag) esp_log_level_set((tag), ESP_LOG_INFO)

void app_logging_init(void)
{
#if APP_LOG_KEEP_UNSELECTED_ERRORS
    esp_log_level_set("*", ESP_LOG_ERROR);
#else
    esp_log_level_set("*", ESP_LOG_NONE);
#endif

#if APP_LOG_ENABLE_CPU_POWER
    ENABLE_INFO("PWR_CPU");
    ENABLE_INFO("PWR_STATE");
#endif

#if APP_LOG_ENABLE_ESP_PM
    ENABLE_INFO("pm");
#endif

#if APP_LOG_ENABLE_POWER_SLEEP
    ENABLE_INFO("app_sleep");
    ENABLE_INFO("light_sleep");
    ENABLE_INFO("deep_sleep");
    ENABLE_INFO("wake_up");
    ENABLE_INFO("component_audio");
    ENABLE_INFO("component_display");
    ENABLE_INFO("component_ethernet");
    ENABLE_INFO("component_sdcard");
    ENABLE_INFO("component_wifi");
    ENABLE_INFO("POWER_PROFILE");
    /*
     * Pre-Deep-sleep rail audit. Without this line the whole audit table is
     * swallowed by the esp_log_level_set("*", ESP_LOG_NONE) default above.
     */
    ENABLE_INFO("PWR_AUDIT");
#endif

#if APP_LOG_ENABLE_APPLICATION
    ENABLE_INFO("app_main");
#endif

#if APP_LOG_ENABLE_CAMERA
    ENABLE_INFO("app_video");
    ENABLE_INFO("ov5647");
    ENABLE_INFO("esp_video");
#endif

#if APP_LOG_ENABLE_DISPLAY
    ENABLE_INFO("ESP32_P4_PLATFORM");
    ENABLE_INFO("esp_lvgl:adapter");
    ENABLE_INFO("esp_lvgl:bridge");
    ENABLE_INFO("esp_lvgl:bridge_v9");
    ENABLE_INFO("esp_lvgl:touch");
    ENABLE_INFO("jd9365");
    ENABLE_INFO("GT911");
#endif

#if APP_LOG_ENABLE_AI
    ENABLE_INFO("face_detect_wrapper");
    ENABLE_INFO("face_recognition_wrapper");
    ENABLE_INFO("FbsLoader");
    ENABLE_INFO("dl::Model");
#endif

#if APP_LOG_ENABLE_UI
    ENABLE_INFO("app_ui");
    ENABLE_INFO("pin_screen");
#endif
}

#undef ENABLE_INFO
