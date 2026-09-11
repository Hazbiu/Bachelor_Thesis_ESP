#include "services/vision/ai_snapshot_buffer.h"

#include "config/app_config.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "services/vision/ai_snapshot.h"


static uint8_t *s_snapshot_buffer = NULL;
static size_t s_snapshot_capacity = 0;
static size_t s_snapshot_alignment = 0;


static size_t snapshot_align_up(
    size_t value,
    size_t alignment)
{
    return (value + (alignment - 1U)) & ~(alignment - 1U);
}


esp_err_t vision_ai_snapshot_buffer_init(size_t alignment)
{
    if (s_snapshot_buffer != NULL) {
        return ESP_OK;
    }

    const size_t raw_capacity =
        (size_t)APP_AI_SNAPSHOT_MAX_EDGE *
        APP_AI_SNAPSHOT_MAX_EDGE *
        vision_ai_snapshot_bytes_per_pixel();

    s_snapshot_capacity =
        snapshot_align_up(raw_capacity, alignment);

    s_snapshot_buffer = heap_caps_aligned_calloc(
        alignment,
        1,
        s_snapshot_capacity,
        MALLOC_CAP_SPIRAM);

    if (s_snapshot_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_snapshot_alignment = alignment;
    return ESP_OK;
}


uint8_t *vision_ai_snapshot_buffer_data(void)
{
    return s_snapshot_buffer;
}


size_t vision_ai_snapshot_buffer_capacity(void)
{
    return s_snapshot_capacity;
}


void vision_ai_snapshot_buffer_sync_cpu_to_memory(size_t data_size)
{
    if (s_snapshot_buffer == NULL || s_snapshot_alignment == 0) {
        return;
    }

    const size_t sync_size =
        snapshot_align_up(data_size, s_snapshot_alignment);

    (void)esp_cache_msync(
        s_snapshot_buffer,
        sync_size,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}


void vision_ai_snapshot_buffer_sync_memory_to_cpu(size_t data_size)
{
    if (s_snapshot_buffer == NULL ||
        s_snapshot_alignment == 0 ||
        data_size == 0) {
        return;
    }

    const size_t sync_size =
        snapshot_align_up(data_size, s_snapshot_alignment);

    (void)esp_cache_msync(
        s_snapshot_buffer,
        sync_size,
        ESP_CACHE_MSYNC_FLAG_DIR_M2C);
}
