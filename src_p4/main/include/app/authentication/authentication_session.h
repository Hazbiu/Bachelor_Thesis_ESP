
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Thread-safe authentication-session state.
 *
 * Owns:
 * - PIN transition pending
 * - PIN screen active
 * - face-leave rearm requirement
 * - pending recognized identity
 *
 * Does not control camera hardware, LVGL, CPU frequency,
 * application FSM transitions, or sleep hardware.
 */

bool authentication_session_blocks_camera(void);

bool authentication_session_note_detection_result(
    int face_count);

bool authentication_session_reserve_pin_identity(
    const char *recognized_name);

void authentication_session_copy_pending_identity(
    char *destination,
    size_t destination_size);

void authentication_session_mark_transition_failed(void);

void authentication_session_mark_pin_active(void);

void authentication_session_mark_camera_active_after_pin(void);

/*
 * Fully reset authentication state.
 *
 * Used by the existing sleep/display-suspend paths.
 */
void authentication_session_reset(void);

#ifdef __cplusplus
}
#endif
