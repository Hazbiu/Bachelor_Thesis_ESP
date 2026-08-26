#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Apply the system-wide Ethernet policy immediately.
 * Disabled sets IP101GRI BMCR Power Down; enabled restores normal operation.
 */
esp_err_t component_ethernet_set_enabled(bool enabled);
bool component_ethernet_is_enabled(void);

/**
 * Reversible Light-sleep path: preserve the project's previous behavior by
 * holding the onboard IP101GRI PHY in hardware reset.
 *
 * ESP32-P4-NANO:
 *   GPIO51 LOW  = PHY reset asserted
 *   GPIO51 HIGH = PHY reset released
 */
esp_err_t component_ethernet_hold_reset_for_light_sleep(void);
esp_err_t component_ethernet_restore_after_light_sleep(void);

/**
 * Deep-sleep path: release RESET and use Clause-22 MDC/MDIO to set BMCR bit 11
 * (Power Down). The IP101GRI datasheet specifies that this powers down the PHY
 * and its internal crystal while MDC/MDIO remain accessible.
 *
 * RESET is deliberately left released after programming; asserting RESET would
 * restore BMCR defaults and clear the Power Down bit.
 */
esp_err_t component_ethernet_disable_for_deep_sleep(void);

/** Re-read BMCR and verify that bit 11 is still set. */
esp_err_t component_ethernet_verify_power_down(void);

/**
 * Recovery hook used only if esp_deep_sleep_start() unexpectedly returns.
 * Restores the pre-sleep BMCR value captured before Power Down.
 */
esp_err_t component_ethernet_restore_after_failed_sleep(void);

#ifdef __cplusplus
}
#endif

