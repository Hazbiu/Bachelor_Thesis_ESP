#pragma once

#include <stdbool.h>

#include "domain/app_event.h"
#include "domain/app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Notification raised after a successful application state transition.
 *
 * The controller owns state-machine interaction.
 * Higher-level integration can later observe transitions without
 * the state machine knowing anything about UI, hardware or services.
 */
typedef void (*app_controller_transition_callback_t)(
    app_state_t previous_state,
    app_event_type_t event,
    app_state_t new_state,
    void *user_data);


/*
 * Initialize the application controller.
 *
 * Initial state:
 *     APP_STATE_BOOTING
 *
 * The transition callback is optional.
 */
void app_controller_init(
    app_controller_transition_callback_t transition_callback,
    void *user_data);


/*
 * Dispatch one high-level application event.
 *
 * Returns true only when the event causes a valid state transition.
 */
bool app_controller_handle_event(app_event_type_t event);


/*
 * Return the current high-level application state.
 */
app_state_t app_controller_get_state(void);


/*
 * Return true after app_controller_init() has been called.
 */
bool app_controller_is_initialized(void);


#ifdef __cplusplus
}
#endif
