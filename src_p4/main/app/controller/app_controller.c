#include "app/controller/app_controller.h"

#include <stddef.h>

#include "app/state/app_state_machine.h"
#include "freertos/FreeRTOS.h"


static app_state_machine_t s_state_machine;

static app_controller_transition_callback_t s_transition_callback;
static void *s_transition_user_data;

static bool s_initialized;

/*
 * Controller events may originate from different FreeRTOS tasks/cores:
 *
 *   CPU0 / system tasks:
 *       boot
 *       launcher navigation
 *       settings navigation
 *       PIN/display transitions
 *
 *   CPU1 / AI worker:
 *       recognition/authentication events
 *
 * Protect only the small in-memory controller state. Hardware operations,
 * UI operations and callbacks are never executed while this lock is held.
 */
static portMUX_TYPE s_controller_lock = portMUX_INITIALIZER_UNLOCKED;


void app_controller_init(
    app_controller_transition_callback_t transition_callback,
    void *user_data)
{
    portENTER_CRITICAL(&s_controller_lock);

    app_state_machine_init(
        &s_state_machine,
        APP_STATE_BOOTING);

    s_transition_callback = transition_callback;
    s_transition_user_data = user_data;

    s_initialized = true;

    portEXIT_CRITICAL(&s_controller_lock);
}


bool app_controller_handle_event(app_event_type_t event)
{
    app_controller_transition_callback_t callback = NULL;
    void *callback_user_data = NULL;

    app_state_t previous_state = APP_STATE_BOOTING;
    app_state_t new_state = APP_STATE_BOOTING;

    bool transitioned = false;

    /*
     * The state-machine operation itself is intentionally tiny and pure,
     * therefore it is safe to serialize it with the controller spinlock.
     */
    portENTER_CRITICAL(&s_controller_lock);

    if (s_initialized) {
        previous_state =
            app_state_machine_get_state(&s_state_machine);

        transitioned =
            app_state_machine_handle_event(
                &s_state_machine,
                event);

        if (transitioned) {
            new_state =
                app_state_machine_get_state(&s_state_machine);

            callback = s_transition_callback;
            callback_user_data = s_transition_user_data;
        }
    }

    portEXIT_CRITICAL(&s_controller_lock);

    /*
     * Never invoke application callbacks while holding the controller lock.
     * A callback may later perform logging, queue work or dispatch another
     * application event.
     */
    if (transitioned && callback != NULL) {
        callback(
            previous_state,
            event,
            new_state,
            callback_user_data);
    }

    return transitioned;
}


app_state_t app_controller_get_state(void)
{
    app_state_t state = APP_STATE_BOOTING;

    portENTER_CRITICAL(&s_controller_lock);

    if (s_initialized) {
        state =
            app_state_machine_get_state(&s_state_machine);
    }

    portEXIT_CRITICAL(&s_controller_lock);

    return state;
}


bool app_controller_is_initialized(void)
{
    bool initialized;

    portENTER_CRITICAL(&s_controller_lock);
    initialized = s_initialized;
    portEXIT_CRITICAL(&s_controller_lock);

    return initialized;
}
