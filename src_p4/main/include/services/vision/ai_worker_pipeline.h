#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Application-side ports used by the CPU1 Vision pipeline.
 *
 * The Vision service owns detector/recognizer execution, worker-state
 * synchronization, result processing and the exact CPU1 sequence.
 *
 * It deliberately does NOT own:
 * - application FSM transitions;
 * - display idle-scan state;
 * - PIN reservation/launch;
 * - power/sleep policy;
 * - authentication-session state;
 * - the application-level face-boost acquire/release wrappers.
 *
 * Those concerns are supplied as callbacks so dependency direction remains:
 *
 *     app -> services -> platform
 *
 * rather than services depending upward on the app layer.
 */
typedef struct {
    bool (*idle_scan_display_is_suspended)(void);
    bool (*sleep_is_requested)(void);
    bool (*camera_processing_blocked)(void);
    bool (*note_detection_result)(int face_count);
    esp_err_t (*acquire_face_boost)(void);
    void (*notify_activity)(void);
    void (*recognition_started)(void);
    bool (*request_pin)(const char *recognized_name);
    void (*recognition_not_authenticated)(void);
    bool (*launch_pin_transition)(void);
    esp_err_t (*release_face_boost)(const char *reason);
} vision_ai_worker_pipeline_hooks_t;


/*
 * Start the existing single CPU1 worker using the unchanged task name,
 * stack size, priority and CPU core.
 */
bool vision_ai_worker_pipeline_start(
    const vision_ai_worker_pipeline_hooks_t *hooks);

#ifdef __cplusplus
}
#endif
