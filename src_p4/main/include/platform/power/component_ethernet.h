
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
 * Enabled releases RESET (GPIO51 HIGH). On first application after boot, or
 * after Deep-sleep preparation, a reset pulse clears retained BMCR power-down.
 */
esp_err_t component_ethernet_set_enabled(bool enabled);
bool component_ethernet_is_enabled(void);

/**
 * Reversible Light-sleep PHY shutdown (legacy function name retained).
 * Default V3 policy holds IP101GRI RESET LOW for the full Light-sleep window
 * and performs a clean reset/release on wake. This minimizes external-PHY
 * current without changing touch or button wake sources.
 *
 * APP_LIGHT_SLEEP_ETHERNET_RESET_LOW=0 keeps the BMCR comparison path; any
 * failure there falls back to RESET LOW and remains nonfatal if that fallback
 * succeeds. No Ethernet wake source is configured by this application.
 */
esp_err_t component_ethernet_enter_light_sleep_reduced_mode(void);
esp_err_t component_ethernet_restore_after_light_sleep(void);

/**
 * Deep-sleep: default V3 policy keeps IP101GRI in RESET LOW. The optional
 * APP_PWR_ETHERNET_DEEP_BMCR_POWER_DOWN=1 comparison path uses MDIO/BMCR and
 * falls back to RESET LOW if verification fails.
 */
esp_err_t component_ethernet_disable_for_deep_sleep(void);

/** Verify the selected state; attempt RESET LOW on a failed BMCR audit. */
esp_err_t component_ethernet_verify_power_down(void);

/** Expected RESET level and state name for the final audit/log. */
int component_ethernet_deep_sleep_reset_level(void);
const char *component_ethernet_deep_sleep_state(void);

/**
 * Recovery hook used only if esp_deep_sleep_start() unexpectedly returns.
 * Clears BMCR power-down when saved policy is ON; otherwise keeps RESET LOW.
 */
esp_err_t component_ethernet_restore_after_failed_sleep(void);

#ifdef __cplusplus
}
#endif
