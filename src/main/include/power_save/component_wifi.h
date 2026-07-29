#pragma once

#include "esp_err.h"

/**
 * Disable the external ESP32-C6 Wi-Fi coprocessor before ESP32-P4 deep sleep.
 *
 * Waveshare ESP32-P4-NANO:
 * ESP32-P4 GPIO54 -> ESP32-C6 CHIP_PU
 */
esp_err_t component_wifi_disable_for_deep_sleep(void);

/**
 * Re-enable the ESP32-C6 only if entering deep sleep unexpectedly fails.
 */
esp_err_t component_wifi_restore_after_failed_sleep(void);
