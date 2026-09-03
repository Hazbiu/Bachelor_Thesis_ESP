#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEEP_SLEEP_PROFILE_AGGRESSIVE = 0,
    DEEP_SLEEP_PROFILE_LIGHT_COMPATIBLE,
} deep_sleep_profile_t;

/**
 * Enter ESP32-P4 Deep-sleep using an explicit peripheral-boundary strategy.
 *
 * AGGRESSIVE:
 *   Existing Deep-only behavior: full external-peripheral shutdown plus final
 *   GPIO/I2C cleanup.
 *
 * LIGHT_COMPATIBLE:
 *   Hybrid Light->Deep handoff. Preserve the already-measured low-current
 *   Light-sleep external-peripheral state and only transition the P4 itself
 *   into Deep-sleep. The C6 is still explicitly prepared for self-Deep-sleep.
 */
void enter_deep_sleep_with_profile(deep_sleep_profile_t profile);

/* Existing callers retain the original aggressive behavior. */
void enter_deep_sleep(void);

#ifdef __cplusplus
}
#endif
