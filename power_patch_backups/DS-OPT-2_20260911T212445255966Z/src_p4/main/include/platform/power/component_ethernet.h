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
 * Reversible Light-sleep path: keep the IP101GRI powered and force its standard
 * MII Control Register to 10 Mbps instead of asserting hardware RESET.
 *
 * GPIO51 stays HIGH. The original BMCR value is saved before the transition and
 * restored on application wake. Ethernet is therefore reduced, not stopped.
 *
 * Ethernet traffic is NOT configured as a P4 wake source by this application;
 * touch/GPIO3 remain the application wake sources.
 */
esp_err_t component_ethernet_enter_light_sleep_reduced_mode(void);
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
