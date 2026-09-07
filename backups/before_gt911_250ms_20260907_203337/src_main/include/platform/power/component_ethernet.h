#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Apply the system-wide Ethernet policy immediately.
 *
 * Disabled keeps the IP101GRI in hardware RESET (GPIO51 LOW).
 * Enabled releases RESET (GPIO51 HIGH).
 *
 * Hardware RESET is intentionally used as the low-current policy because the
 * measured Hybrid Light->Deep transition showed higher current when Deep-sleep
 * released RESET and relied on BMCR Power Down.
 */
esp_err_t component_ethernet_set_enabled(bool enabled);
bool component_ethernet_is_enabled(void);

/**
 * Reversible Light-sleep path: hold the onboard IP101GRI PHY in hardware reset.
 *
 * ESP32-P4-NANO:
 *   GPIO51 LOW  = PHY reset asserted
 *   GPIO51 HIGH = PHY reset released
 */
esp_err_t component_ethernet_hold_reset_for_light_sleep(void);
esp_err_t component_ethernet_restore_after_light_sleep(void);

/**
 * Deep-sleep path: preserve the same proven Light-sleep electrical state by
 * re-applying and holding GPIO51 LOW.
 *
 * Deep-sleep wake is a reboot, so there is no need to keep BMCR state alive
 * across sleep. Keeping RESET asserted also avoids waking the PHY for an MDIO
 * transaction during the Light->Deep transition.
 */
esp_err_t component_ethernet_disable_for_deep_sleep(void);

/** Verify that GPIO51 still reads LOW immediately before Deep-sleep. */
esp_err_t component_ethernet_verify_power_down(void);

/**
 * Recovery hook used only if esp_deep_sleep_start() unexpectedly returns.
 * Releases RESET when the saved Ethernet policy is ON; otherwise keeps it LOW.
 */
esp_err_t component_ethernet_restore_after_failed_sleep(void);

#ifdef __cplusplus
}
#endif
