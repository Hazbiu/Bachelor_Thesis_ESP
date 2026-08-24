#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Disable the board's external audio power amplifier before deep sleep.
 *
 * ESP32-P4-NANO:
 * GPIO53 HIGH = amplifier enabled
 * GPIO53 LOW  = amplifier disabled
 */
esp_err_t component_audio_disable_for_deep_sleep(void);

/**
 * Restore the amplifier only when entering deep sleep unexpectedly fails.
 */
esp_err_t component_audio_restore_after_failed_sleep(void);

#ifdef __cplusplus
}
#endif
