#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Safely unmount the microSD card and switch its board-level power rail off
 * before ESP32-P4 deep sleep.
 *
 * Waveshare ESP32-P4-NANO:
 * GPIO45 drives the gate of Q1 (AO3401), which supplies SD1_VDD.
 */
esp_err_t component_sdcard_disable_for_deep_sleep(void);

/**
 * Restore microSD power and remount the card only when entering deep sleep
 * unexpectedly fails.
 */
esp_err_t component_sdcard_restore_after_failed_sleep(void);

#ifdef __cplusplus
}
#endif
