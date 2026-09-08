#pragma once

/*
 * Central application log switches.
 *
 * Set a component to 1 to enable its normal informational output. Set it to
 * 0 to silence that component. CPU clock and power-state logging is the
 * default configuration.
 * ESP-ROM and bootloader output occurs before app_logging_init() and is not
 * controlled by these runtime switches.
 */
#define APP_LOG_ENABLE_CPU_POWER             1
#define APP_LOG_ENABLE_ESP_PM                0
#define APP_LOG_ENABLE_POWER_SLEEP 1
#define APP_LOG_ENABLE_APPLICATION           1
#define APP_LOG_ENABLE_CAMERA                0
#define APP_LOG_ENABLE_DISPLAY               0
#define APP_LOG_ENABLE_AI                    1
#define APP_LOG_ENABLE_UI                    0

/*
 * When enabled, unselected components may still print ESP_LOGE messages.
 * Keep this at 0 for strict CPU-only output; set it to 1 for safer debugging.
 */
#define APP_LOG_KEEP_UNSELECTED_ERRORS       1

/* These monitors use printf(), so ESP-IDF tag filtering cannot silence them. */
#define APP_DIAGNOSTICS_CPU_STATS_ENABLED    0
#define APP_DIAGNOSTICS_AI_PIPELINE_ENABLED  0
