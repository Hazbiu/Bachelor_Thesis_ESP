#pragma once

#include "esp_err.h"

/**
 * Disable the external ESP32-C6 Wi-Fi coprocessor by driving CHIP_PU LOW and
 * holding the GPIO configuration. The call is idempotent, so it is safe to
 * call once during boot and again from the Deep-sleep shutdown sequence.
 *
 * Waveshare ESP32-P4-NANO:
 * ESP32-P4 GPIO54 -> ESP32-C6 CHIP_PU
 */
esp_err_t component_wifi_disable_for_deep_sleep(void);

/**
 * Deep-sleep failure hook. In this build the ESP32-C6 intentionally remains
 * disabled, so this function does not release CHIP_PU or re-enable the C6.
 */
esp_err_t component_wifi_restore_after_failed_sleep(void);
