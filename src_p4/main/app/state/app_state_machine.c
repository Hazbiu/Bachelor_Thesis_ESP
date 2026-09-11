#include "app/state/app_state_machine.h"

#include <stddef.h>


void app_state_machine_init(
    app_state_machine_t *machine,
    app_state_t initial_state)
{
    if (machine == NULL) {
        return;
    }

    machine->current_state = initial_state;
}


app_state_t app_state_machine_get_state(
    const app_state_machine_t *machine)
{
    if (machine == NULL) {
        return APP_STATE_BOOTING;
    }

    return machine->current_state;
}


bool app_state_machine_handle_event(
    app_state_machine_t *machine,
    app_event_type_t event)
{
    if (machine == NULL) {
        return false;
    }

    app_state_t next_state = machine->current_state;

    switch (machine->current_state) {

    case APP_STATE_BOOTING:
        if (event == APP_EVENT_BOOT_COMPLETE) {
            next_state = APP_STATE_LAUNCHER;
        }
        break;


    case APP_STATE_LAUNCHER:
        if (event == APP_EVENT_START_CAMERA) {
            next_state = APP_STATE_CAMERA_ACTIVE;
        }
        else if (event == APP_EVENT_OPEN_SETTINGS) {
            next_state = APP_STATE_SETTINGS;
        }
        else if (event == APP_EVENT_DEEP_SLEEP_REQUEST) {
            next_state = APP_STATE_DEEP_SLEEP;
        }
        break;


    case APP_STATE_SETTINGS:
        if (event == APP_EVENT_SETTINGS_BACK) {
            next_state = APP_STATE_LAUNCHER;
        }
        else if (event == APP_EVENT_DEEP_SLEEP_REQUEST) {
            next_state = APP_STATE_DEEP_SLEEP;
        }
        break;


    case APP_STATE_CAMERA_ACTIVE:
        if (event == APP_EVENT_FACE_DETECTED) {
            next_state = APP_STATE_AUTHENTICATING;
        }
        else if (event == APP_EVENT_LIGHT_SLEEP_REQUEST) {
            next_state = APP_STATE_LIGHT_SLEEP;
        }
        else if (event == APP_EVENT_DEEP_SLEEP_REQUEST) {
            next_state = APP_STATE_DEEP_SLEEP;
        }
        break;


    case APP_STATE_AUTHENTICATING:
        if (event == APP_EVENT_FACE_RECOGNIZED) {
            next_state = APP_STATE_PIN_ENTRY;
        }
        else if (event == APP_EVENT_FACE_UNKNOWN) {
            next_state = APP_STATE_CAMERA_ACTIVE;
        }
        else if (event == APP_EVENT_DEEP_SLEEP_REQUEST) {
            next_state = APP_STATE_DEEP_SLEEP;
        }
        break;


    case APP_STATE_PIN_ENTRY:
        /*
         * PIN entry owns the display until authentication completes. Automatic
         * Light/Deep inactivity transitions are intentionally blocked here and
         * restart only after APP_EVENT_PIN_COMPLETE restores CAMERA_ACTIVE.
         */
        if (event == APP_EVENT_PIN_COMPLETE) {
            next_state = APP_STATE_CAMERA_ACTIVE;
        }
        break;


    case APP_STATE_LIGHT_SLEEP:
        if (event == APP_EVENT_WAKE) {
            next_state = APP_STATE_CAMERA_ACTIVE;
        }
        else if (event == APP_EVENT_DEEP_SLEEP_REQUEST) {
            next_state = APP_STATE_DEEP_SLEEP;
        }
        break;


    case APP_STATE_DEEP_SLEEP:
        /*
         * Deep sleep is intentionally terminal from the application's
         * high-level perspective for now.
         */
        break;


    default:
        return false;
    }


    if (next_state == machine->current_state) {
        return false;
    }

    machine->current_state = next_state;
    return true;
}


const char *app_state_machine_state_name(app_state_t state)
{
    switch (state) {
    case APP_STATE_BOOTING:
        return "BOOTING";

    case APP_STATE_LAUNCHER:
        return "LAUNCHER";

    case APP_STATE_SETTINGS:
        return "SETTINGS";

    case APP_STATE_CAMERA_ACTIVE:
        return "CAMERA_ACTIVE";

    case APP_STATE_AUTHENTICATING:
        return "AUTHENTICATING";

    case APP_STATE_PIN_ENTRY:
        return "PIN_ENTRY";

    case APP_STATE_LIGHT_SLEEP:
        return "LIGHT_SLEEP";

    case APP_STATE_DEEP_SLEEP:
        return "DEEP_SLEEP";

    default:
        return "UNKNOWN_STATE";
    }
}


const char *app_state_machine_event_name(app_event_type_t event)
{
    switch (event) {
    case APP_EVENT_NONE:
        return "NONE";

    case APP_EVENT_BOOT_COMPLETE:
        return "BOOT_COMPLETE";

    case APP_EVENT_START_CAMERA:
        return "START_CAMERA";

    case APP_EVENT_OPEN_SETTINGS:
        return "OPEN_SETTINGS";

    case APP_EVENT_SETTINGS_BACK:
        return "SETTINGS_BACK";

    case APP_EVENT_FACE_DETECTED:
        return "FACE_DETECTED";

    case APP_EVENT_FACE_RECOGNIZED:
        return "FACE_RECOGNIZED";

    case APP_EVENT_FACE_UNKNOWN:
        return "FACE_UNKNOWN";

    case APP_EVENT_PIN_COMPLETE:
        return "PIN_COMPLETE";

    case APP_EVENT_INACTIVITY:
        return "INACTIVITY";

    case APP_EVENT_LIGHT_SLEEP_REQUEST:
        return "LIGHT_SLEEP_REQUEST";

    case APP_EVENT_DEEP_SLEEP_REQUEST:
        return "DEEP_SLEEP_REQUEST";

    case APP_EVENT_WAKE:
        return "WAKE";

    default:
        return "UNKNOWN_EVENT";
    }
}
