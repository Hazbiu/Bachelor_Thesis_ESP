#pragma once

#include <stdbool.h>

#include "domain/app_event.h"
#include "domain/app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pure application state machine.
 *
 * Responsibilities:
 * - store the current high-level application state
 * - evaluate an application event
 * - perform only valid state transitions
 *
 * This module performs NO hardware operations.
 * It does not know about ESP-IDF, LVGL, camera drivers,
 * power components or AI backends.
 */
typedef struct
{
    app_state_t current_state;
} app_state_machine_t;


/*
 * Initialize the state machine with an explicit initial state.
 */
void app_state_machine_init(
    app_state_machine_t *machine,
    app_state_t initial_state);


/*
 * Return the current application state.
 */
app_state_t app_state_machine_get_state(
    const app_state_machine_t *machine);


/*
 * Process one application event.
 *
 * Returns:
 *   true  -> a valid transition occurred
 *   false -> the event does not cause a transition
 *
 * No side effects outside the state machine occur here.
 */
bool app_state_machine_handle_event(
    app_state_machine_t *machine,
    app_event_type_t event);


/*
 * Human-readable names intended for logging and diagnostics.
 */
const char *app_state_machine_state_name(app_state_t state);
const char *app_state_machine_event_name(app_event_type_t event);


#ifdef __cplusplus
}
#endif
