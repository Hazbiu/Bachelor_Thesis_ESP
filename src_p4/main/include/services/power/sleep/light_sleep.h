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
 * Only the RTC timer is enabled. The application polls GT911 between
 * finite timer slices. GPIO3 is reserved for real Deep-sleep wake-up.
 * enable_gpio_wakeup is retained for compatibility and must be false.
 * Passing true or a zero timeout returns ESP_ERR_INVALID_ARG.
 *
 * This low-level helper configures the documented ESP-IDF power-domain policy,
 * wake sources, enters Light-sleep, records the wake cause, and removes those
 * wake sources again after wake. The application owns reversible board-level
 * camera/display/audio/SD/Ethernet suspend-resume policy.
 *
 * @param timeout_ms Nonzero timer wake-up interval in milliseconds.
 * @param enable_gpio_wakeup Must be false; the rocker is Deep-sleep-only.
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
