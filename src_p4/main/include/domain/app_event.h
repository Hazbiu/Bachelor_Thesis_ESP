#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * High-level events that can cause application state transitions.
 *
 * Events describe WHAT HAPPENED.
 * The application controller will later decide what should happen next.
 */
typedef enum
{
    APP_EVENT_NONE = 0,

    APP_EVENT_BOOT_COMPLETE,

    APP_EVENT_START_CAMERA,

    APP_EVENT_OPEN_SETTINGS,

    APP_EVENT_SETTINGS_BACK,

    APP_EVENT_FACE_DETECTED,

    APP_EVENT_FACE_RECOGNIZED,

    APP_EVENT_FACE_UNKNOWN,

    APP_EVENT_PIN_COMPLETE,

    APP_EVENT_INACTIVITY,

    APP_EVENT_LIGHT_SLEEP_REQUEST,

    APP_EVENT_DEEP_SLEEP_REQUEST,

    APP_EVENT_WAKE

} app_event_type_t;

#ifdef __cplusplus
}
#endif
