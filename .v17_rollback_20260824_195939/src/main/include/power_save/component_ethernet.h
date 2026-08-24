#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Hold the onboard IP101GRI Ethernet PHY in hardware reset.
 *
 * ESP32-P4-NANO:
 * GPIO51 LOW  = PHY held in reset
 * GPIO51 HIGH = PHY released
 */
esp_err_t component_ethernet_disable_for_deep_sleep(void);

/**
 * Release the PHY only if entering deep sleep unexpectedly fails.
 */
esp_err_t component_ethernet_restore_after_failed_sleep(void);

#ifdef __cplusplus
}
#endif
