#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Metadata for exactly one AI snapshot.
 *
 * The image bytes themselves remain owned by app_main's existing
 * ai_snapshot_buffer. This object contains metadata only.
 */
typedef struct
{
    uint32_t frame_id;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t width;
    uint32_t height;
    size_t data_size;
    bool idle_scan_frame;
} vision_ai_worker_job_t;


/*
 * Reserve the single AI job slot.
 *
 * Returns true only when:
 * - the worker is accepting jobs,
 * - it is not currently busy,
 * - no job is already pending.
 *
 * This preserves the existing latest-frame/no-backlog policy.
 */
bool vision_ai_worker_state_try_reserve(void);


/*
 * Cancel a previously reserved slot.
 *
 * Used when snapshot preparation fails before the worker is notified.
 */
void vision_ai_worker_state_cancel_reservation(void);


/*
 * Store metadata into the currently reserved slot.
 *
 * Intentionally does NOT force the pending flag back to true.
 * This preserves the previous shutdown race semantics:
 * if sleep cancels the pending job while snapshot copying is in progress,
 * committing metadata does not resurrect that cancelled job.
 */
void vision_ai_worker_state_commit_job(
    const vision_ai_worker_job_t *job);


/*
 * Called by the worker after task notification.
 *
 * When a pending job exists:
 * - copy it to out_job,
 * - clear pending,
 * - mark the worker busy,
 * - return true.
 */
bool vision_ai_worker_state_take_job(
    vision_ai_worker_job_t *out_job);


/* Mark the current worker inference cycle complete. */
void vision_ai_worker_state_mark_idle(void);


/*
 * Stop accepting new work and drop any job that has not started yet.
 *
 * A currently running inference is not interrupted.
 */
void vision_ai_worker_state_pause_accepting_and_drop_pending(void);


/*
 * Stop accepting new work, drop a pending-but-not-started job and wait
 * for any currently running worker job to finish.
 *
 * Returns true when the worker becomes idle before timeout_ms.
 */
bool vision_ai_worker_state_pause_and_drain(
    uint32_t timeout_ms);

/* Allow new single-slot jobs again. */
void vision_ai_worker_state_resume_accepting(void);


/* Return true while the worker owns an active job. */
bool vision_ai_worker_state_is_busy(void);

#ifdef __cplusplus
}
#endif
