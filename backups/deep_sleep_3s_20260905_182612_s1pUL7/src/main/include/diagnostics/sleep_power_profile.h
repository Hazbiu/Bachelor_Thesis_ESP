#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Serialized by app_sleep's existing sleep-request owner. Inactive during
 * normal operation and Light-sleep, including runtime component toggles. */
void sleep_power_profile_start(const char *origin);
void sleep_power_profile_stop(void);
void sleep_power_profile_before(const char *component);
void sleep_power_profile_after(const char *component, const char *result);

/* Announce the last observable boundary before detaching the UART pins.
 * quiet_wait() performs the same measurement delay without emitting logs. */
void sleep_power_profile_final_boundary(bool detach_uart);
void sleep_power_profile_quiet_wait(void);

#ifdef __cplusplus
}
#endif
