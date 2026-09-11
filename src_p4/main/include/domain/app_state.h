#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * High-level application states.
 *
 * These states describe WHAT the application is currently doing.
 * They do not contain hardware-specific details.
 */
typedef enum
{
    APP_STATE_BOOTING = 0,

    APP_STATE_LAUNCHER,

    APP_STATE_SETTINGS,

    APP_STATE_CAMERA_ACTIVE,

    APP_STATE_AUTHENTICATING,

    APP_STATE_PIN_ENTRY,

    APP_STATE_LIGHT_SLEEP,

    APP_STATE_DEEP_SLEEP

} app_state_t;

#ifdef __cplusplus
}
#endif
