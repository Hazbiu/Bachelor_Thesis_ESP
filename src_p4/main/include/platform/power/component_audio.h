#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Reversible Light-sleep path:
 *   1. disable and hold the external NS4150B amplifier OFF;
 *   2. suspend the ES8311 ADC/DAC/analog/reference/clock blocks with the same
 *      register sequence used by Espressif's ES8311 driver;
 *   3. verify the programmed codec registers by I2C read-back.
 *
 * The exact pre-sleep ES8311 register state is snapshotted and restored on
 * wake. If Audio was already OFF before Light-sleep, wake keeps it OFF.
 */
esp_err_t component_audio_disable_for_light_sleep(void);
esp_err_t component_audio_restore_after_light_sleep(void);

/**
 * Deep-sleep path:
 *   1. disable the external NS4150B amplifier (GPIO53 LOW);
 *   2. suspend the onboard ES8311 codec with Espressif's ES8311 sequence;
 *   3. verify the final codec state by I2C read-back.
 *
 * The ES8311 is accessed through the board's shared BSP I2C bus.
 */
esp_err_t component_audio_disable_for_deep_sleep(void);

/**
 * Re-read the ES8311 registers used by the suspend sequence and confirm that
 * the codec still matches the expected low-power state.
 */
esp_err_t component_audio_verify_power_down(void);

/**
 * Restore the last ES8311 register snapshot and release the NS4150B shutdown.
 * This is used by the existing runtime Audio=ON policy and as recovery if a
 * Deep-sleep transition unexpectedly returns.
 */
esp_err_t component_audio_restore_after_failed_sleep(void);

#ifdef __cplusplus
}
#endif
