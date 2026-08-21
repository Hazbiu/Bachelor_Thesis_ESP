#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_sleep.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Enter ESP32-P4 Light-sleep and resume in place after wake-up.
 *
 * If enable_gpio_wakeup is true, APP_LIGHT_SLEEP_WAKE_GPIO is enabled as an
 * active-low wake source. If timeout_ms is greater than zero, the RTC timer is
 * enabled as another wake source. At least one wake source must be requested.
 *
 * This low-level helper only configures wake sources and enters Light-sleep.
 * The application owns the reversible camera/display suspend-resume policy.
 *
 * @param timeout_ms Optional timer wake-up in milliseconds; 0 disables it.
 * @param enable_gpio_wakeup Enable active-low GPIO wake-up when true.
 * @return ESP_OK after a successful sleep/wake cycle, otherwise an ESP-IDF
 *         error returned while configuring, entering, or cleaning up sleep.
 */
esp_err_t enter_light_sleep(uint32_t timeout_ms, bool enable_gpio_wakeup);

/**
 * Enter one quiet Light-sleep polling slice.
 *
 * Configuration failures are still logged, but normal timer entry/wake lines
 * are suppressed so 250 ms touchscreen polling does not flood the monitor.
 */
esp_err_t enter_light_sleep_poll_slice(
    uint32_t timeout_ms,
    bool enable_gpio_wakeup);

/** Return the wake-up cause recorded by the most recent Light-sleep cycle. */
esp_sleep_wakeup_cause_t light_sleep_get_last_wakeup_cause(void);

#ifdef __cplusplus
}
#endif
