#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Submit one camera frame to the asynchronous AI worker.
 *
 * This preserves the existing single-slot/latest-frame behavior:
 * - reject when the worker or snapshot buffer is unavailable;
 * - reserve the single worker slot;
 * - calculate snapshot dimensions;
 * - copy/downscale into the owned snapshot buffer;
 * - cancel the reservation if copying fails;
 * - synchronize CPU writes to memory;
 * - commit exactly one job;
 * - notify the existing CPU1 worker.
 *
 * Detection cadence remains owned by the camera/application coordinator.
 */
bool vision_ai_snapshot_scheduler_schedule(
    const uint8_t *camera_buf,
    size_t camera_buf_len,
    uint32_t camera_width,
    uint32_t camera_height,
    uint32_t current_frame,
    bool idle_scan_frame);

#ifdef __cplusplus
}
#endif
