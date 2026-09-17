#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Apply the system-wide Wi-Fi policy immediately.
 * Enabled releases ESP32-C6 CHIP_PU; disabled holds CHIP_PU LOW.
 */
esp_err_t component_wifi_set_enabled(bool enabled);
bool component_wifi_is_enabled(void);

/**
 * Reversible P4 Light-sleep preparation.
 *
 * Uses the schematic-wired P4 GPIO6 -> C6 GPIO2 sideband as a live mode request.
 * CHIP_PU stays continuously HIGH and the already-running C6 companion changes
 * from 160 MHz to 80 MHz in place. No C6 reset/power-cycle occurs.
 */
esp_err_t component_wifi_prepare_for_light_sleep(void);

/**
 * Leave the retained C6 80 MHz state after P4 Light-sleep.
 * The sideband returns LOW; if the saved Wi-Fi policy is ON the companion
 * firmware returns to 160 MHz, otherwise CHIP_PU is clamped LOW again.
 */
esp_err_t component_wifi_restore_after_light_sleep(void);

/**
 * Drive and hold the C6 mode sideband LOW for the real Deep-sleep boundary.
 * This establishes the pre-entry sideband state; it does not verify that
 * the external C6 firmware has entered sleep.
 */
esp_err_t component_wifi_prepare_mode_for_deep_sleep(void);

/**
 * Disable the external ESP32-C6 Wi-Fi coprocessor by driving CHIP_PU LOW and
 * holding the GPIO configuration.
 *
 * Unlike the older implementation, every call re-applies the GPIO54
 * configuration instead of returning early just because the C6 was disabled
 * by the saved policy. This matters at the final Deep-sleep boundary.
 *
 * Waveshare ESP32-P4-NANO:
 * ESP32-P4 GPIO54 -> ESP32-C6 CHIP_PU
 */
esp_err_t component_wifi_disable_for_deep_sleep(void);

/**
 * Re-apply GPIO54 LOW at the last possible application-controlled point before
 * esp_deep_sleep_start().
 *
 * This prepares the per-pin clamp. deep_sleep.c additionally arms the
 * PMU sleep-state HP-pad hold on the reviewed old-P4/ESP-IDF configuration.
 * Physical voltage during sleep must be checked on the board.
 */
esp_err_t component_wifi_force_off_at_sleep_boundary(void);

/**
 * Deep-sleep failure hook. Restores the saved system-wide Wi-Fi policy:
 * CHIP_PU HIGH when Wi-Fi is enabled, or LOW when it is disabled.
 */
esp_err_t component_wifi_restore_after_failed_sleep(void);

#ifdef __cplusplus
}
#endif
