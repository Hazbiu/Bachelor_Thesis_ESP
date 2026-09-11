#include "services/vision/ai_snapshot.h"

#include "config/app_config.h"

size_t vision_ai_snapshot_bytes_per_pixel(void)
{
#if APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565
    return 2U;
#else
    return 3U;
#endif
}


void vision_ai_snapshot_dimensions(
    uint32_t source_width,
    uint32_t source_height,
    uint32_t *snapshot_width,
    uint32_t *snapshot_height)
{
    if (snapshot_width == NULL ||
        snapshot_height == NULL ||
        source_width == 0 ||
        source_height == 0) {
        return;
    }

    if (source_width >= source_height) {
        *snapshot_width = APP_AI_SNAPSHOT_MAX_EDGE;

        *snapshot_height =
            (source_height * APP_AI_SNAPSHOT_MAX_EDGE +
             source_width / 2U) /
            source_width;
    } else {
        *snapshot_height = APP_AI_SNAPSHOT_MAX_EDGE;

        *snapshot_width =
            (source_width * APP_AI_SNAPSHOT_MAX_EDGE +
             source_height / 2U) /
            source_height;
    }

    if (*snapshot_width == 0) {
        *snapshot_width = 1;
    }

    if (*snapshot_height == 0) {
        *snapshot_height = 1;
    }
}


bool vision_ai_snapshot_copy(
    const uint8_t *source,
    size_t source_len,
    uint32_t source_width,
    uint32_t source_height,
    uint8_t *destination,
    size_t destination_capacity,
    uint32_t snapshot_width,
    uint32_t snapshot_height,
    size_t *written_bytes)
{
    if (source == NULL ||
        destination == NULL ||
        written_bytes == NULL ||
        source_width == 0 ||
        source_height == 0 ||
        snapshot_width == 0 ||
        snapshot_height == 0) {
        return false;
    }

    const size_t bytes_per_pixel =
        vision_ai_snapshot_bytes_per_pixel();

    const size_t required_source =
        (size_t)source_width *
        source_height *
        bytes_per_pixel;

    const size_t required_snapshot =
        (size_t)snapshot_width *
        snapshot_height *
        bytes_per_pixel;

    if (source_len < required_source ||
        required_snapshot > destination_capacity) {
        return false;
    }

#if APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565

    const uint16_t *src =
        (const uint16_t *)source;

    uint16_t *dst =
        (uint16_t *)destination;

    for (uint32_t y = 0; y < snapshot_height; y++) {
        const uint32_t src_y =
            (uint32_t)(
                ((uint64_t)y * source_height) /
                snapshot_height);

        const uint16_t *src_row =
            src + (size_t)src_y * source_width;

        uint16_t *dst_row =
            dst + (size_t)y * snapshot_width;

        for (uint32_t x = 0; x < snapshot_width; x++) {
            const uint32_t src_x =
                (uint32_t)(
                    ((uint64_t)x * source_width) /
                    snapshot_width);

            dst_row[x] = src_row[src_x];
        }
    }

#else

    for (uint32_t y = 0; y < snapshot_height; y++) {
        const uint32_t src_y =
            (uint32_t)(
                ((uint64_t)y * source_height) /
                snapshot_height);

        for (uint32_t x = 0; x < snapshot_width; x++) {
            const uint32_t src_x =
                (uint32_t)(
                    ((uint64_t)x * source_width) /
                    snapshot_width);

            const size_t src_offset =
                ((size_t)src_y * source_width + src_x) * 3U;

            const size_t dst_offset =
                ((size_t)y * snapshot_width + x) * 3U;

            destination[dst_offset + 0] =
                source[src_offset + 0];

            destination[dst_offset + 1] =
                source[src_offset + 1];

            destination[dst_offset + 2] =
                source[src_offset + 2];
        }
    }

#endif

    *written_bytes = required_snapshot;

    return true;
}
