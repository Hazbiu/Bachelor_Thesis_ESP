#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Reversible Light-sleep path: disable only the external NS4150B amplifier.
 *
 * The ES8311 codec is deliberately left unchanged in Light-sleep so the
 * existing wake/resume path does not have to reconstruct codec clock/state.
 */
esp_err_t component_audio_disable_for_light_sleep(void);
esp_err_t component_audio_restore_after_light_sleep(void);

/**
 * Deep-sleep path:
 *   1. disable the external NS4150B amplifier (GPIO53 LOW);
 *   2. put the onboard ES8311 codec into the same suspend register state used
 *      by Espressif's esp_codec_dev ES8311 driver.
 *
 * The ES8311 is accessed through the board's shared BSP I2C bus.
 */
esp_err_t component_audio_disable_for_deep_sleep(void);

/**
 * Re-read the ES8311 registers used by the suspend sequence and confirm that
 * the codec still matches the expected Deep-sleep state.
 */
esp_err_t component_audio_verify_power_down(void);

/**
 * Recovery hook used only if esp_deep_sleep_start() unexpectedly returns.
 * Restores the ES8311 register snapshot captured before suspend, then releases
 * the NS4150B amplifier shutdown.
 */
esp_err_t component_audio_restore_after_failed_sleep(void);

#ifdef __cplusplus
}
#endif
