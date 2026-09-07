#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Return the byte size of one camera pixel for the currently selected
 * application video format.
 */
size_t vision_ai_snapshot_bytes_per_pixel(void);

/*
 * Calculate the aspect-ratio-preserving AI snapshot dimensions using
 * APP_AI_SNAPSHOT_MAX_EDGE.
 */
void vision_ai_snapshot_dimensions(
    uint32_t source_width,
    uint32_t source_height,
    uint32_t *snapshot_width,
    uint32_t *snapshot_height);

/*
 * Copy and nearest-neighbour downscale one camera frame into the supplied
 * AI snapshot buffer.
 *
 * The caller continues to own the snapshot buffer and synchronization.
 */
bool vision_ai_snapshot_copy(
    const uint8_t *source,
    size_t source_len,
    uint32_t source_width,
    uint32_t source_height,
    uint8_t *destination,
    size_t destination_capacity,
    uint32_t snapshot_width,
    uint32_t snapshot_height,
    size_t *written_bytes);

#ifdef __cplusplus
}
#endif
