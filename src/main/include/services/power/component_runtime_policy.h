#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** True after the BSP shared I2C bus is available for ES8311 access. */
bool component_runtime_audio_policy_ready(void);

/**
 * Apply the persistent Active-mode audio policy.
 *
 * OFF suspends the ES8311 codec and disables the NS4150B amplifier. ON restores
 * the exact codec register snapshot captured while switching it off.
 */
esp_err_t component_runtime_set_audio_enabled(bool enabled);

/**
 * Apply the persistent Active-mode microSD policy.
 *
 * OFF safely unmounts FATFS and removes SD1_VDD. ON restores the GPIO45 rail
 * and mounts the card at /sdcard.
 */
esp_err_t component_runtime_set_sdcard_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
