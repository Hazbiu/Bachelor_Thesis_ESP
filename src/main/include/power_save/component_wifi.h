#pragma once

#include "esp_err.h"

/**
 * Disable the external ESP32-C6 Wi-Fi coprocessor by driving CHIP_PU LOW and
 * holding the GPIO configuration.
 *
 * Unlike the older implementation, every call re-applies the GPIO54
 * configuration instead of returning early just because the C6 was disabled
 * earlier at boot. This matters at the final Deep-sleep boundary.
 *
 * Waveshare ESP32-P4-NANO:
 * ESP32-P4 GPIO54 -> ESP32-C6 CHIP_PU
 */
esp_err_t component_wifi_disable_for_deep_sleep(void);

/**
 * Re-apply GPIO54 LOW at the last possible application-controlled point before
 * esp_deep_sleep_start().
 *
 * This is a best-effort software clamp for ESP32-P4 revision v1.3. GPIO54 is
 * an HP/digital GPIO, so an external pull-down on C6 CHIP_PU is still required
 * for guaranteed LOW level while the HP GPIO domain is powered off.
 */
esp_err_t component_wifi_force_off_at_sleep_boundary(void);

/**
 * Deep-sleep failure hook. In this build the ESP32-C6 intentionally remains
 * disabled, so this function does not release CHIP_PU or re-enable the C6.
 */
esp_err_t component_wifi_restore_after_failed_sleep(void);
