
#include "app/authentication/authentication_session.h"

#include <string.h>

#include "config/app_config.h"
#include "freertos/FreeRTOS.h"
#include "services/vision/face_recognizer.h"

static portMUX_TYPE s_authentication_lock =
    portMUX_INITIALIZER_UNLOCKED;

static bool s_pin_transition_pending = false;
static bool s_pin_screen_active = false;
static bool s_pin_rearm_required = false;
static int s_pin_rearm_no_face_passes = 0;

static char s_pending_identity[FACE_RECOG_MAX_NAME_LEN] = {0};


bool authentication_session_blocks_camera(void)
{
    bool blocked;

    portENTER_CRITICAL(&s_authentication_lock);

    blocked =
        s_pin_transition_pending ||
        s_pin_screen_active;

    portEXIT_CRITICAL(&s_authentication_lock);

    return blocked;
}


bool authentication_session_note_detection_result(
    int face_count)
{
    bool rearmed = false;

    portENTER_CRITICAL(&s_authentication_lock);

    if (face_count > 0) {
        s_pin_rearm_no_face_passes = 0;
    } else if (
        s_pin_rearm_required &&
        !s_pin_transition_pending &&
        !s_pin_screen_active) {

        s_pin_rearm_no_face_passes++;

        if (s_pin_rearm_no_face_passes >=
            APP_FACE_BOX_HOLD_MISSES) {

            s_pin_rearm_required = false;
            s_pin_rearm_no_face_passes = 0;
            rearmed = true;
        }
    }

    portEXIT_CRITICAL(&s_authentication_lock);

    return rearmed;
}


bool authentication_session_reserve_pin_identity(
    const char *recognized_name)
{
    if (recognized_name == NULL ||
        recognized_name[0] == '\0' ||
        strcmp(recognized_name, "unknown") == 0) {
        return false;
    }

    bool reserved = false;

    portENTER_CRITICAL(&s_authentication_lock);

    if (!s_pin_transition_pending &&
        !s_pin_screen_active &&
        !s_pin_rearm_required) {

        s_pin_transition_pending = true;
        s_pin_rearm_required = true;
        s_pin_rearm_no_face_passes = 0;

        const size_t identity_length =
            strnlen(
                recognized_name,
                sizeof(s_pending_identity) - 1);

        memcpy(
            s_pending_identity,
            recognized_name,
            identity_length);

        s_pending_identity[identity_length] = '\0';

        reserved = true;
    }

    portEXIT_CRITICAL(&s_authentication_lock);

    return reserved;
}


void authentication_session_copy_pending_identity(
    char *destination,
    size_t destination_size)
{
    if (destination == NULL ||
        destination_size == 0) {
        return;
    }

    portENTER_CRITICAL(&s_authentication_lock);

    const size_t identity_length =
        strnlen(
            s_pending_identity,
            destination_size - 1);

    memcpy(
        destination,
        s_pending_identity,
        identity_length);

    destination[identity_length] = '\0';

    portEXIT_CRITICAL(&s_authentication_lock);
}


void authentication_session_mark_transition_failed(void)
{
    portENTER_CRITICAL(&s_authentication_lock);

    s_pin_transition_pending = false;
    s_pin_screen_active = false;

    /*
     * Preserve the existing rearm requirement.
     */

    portEXIT_CRITICAL(&s_authentication_lock);
}


void authentication_session_mark_pin_active(void)
{
    portENTER_CRITICAL(&s_authentication_lock);

    s_pin_transition_pending = false;
    s_pin_screen_active = true;

    portEXIT_CRITICAL(&s_authentication_lock);
}


void authentication_session_mark_camera_active_after_pin(void)
{
    portENTER_CRITICAL(&s_authentication_lock);

    s_pin_transition_pending = false;
    s_pin_screen_active = false;

    /*
     * Authentication remains disarmed until the face leaves.
     */

    portEXIT_CRITICAL(&s_authentication_lock);
}


void authentication_session_reset(void)
{
    portENTER_CRITICAL(&s_authentication_lock);

    s_pin_transition_pending = false;
    s_pin_screen_active = false;
    s_pin_rearm_required = false;
    s_pin_rearm_no_face_passes = 0;
    s_pending_identity[0] = '\0';

    portEXIT_CRITICAL(&s_authentication_lock);
}
