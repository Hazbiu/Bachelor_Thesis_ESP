#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Own the application's single PSRAM-backed AI snapshot slot.
 *
 * Allocation semantics are intentionally identical to the former app_main
 * implementation:
 * - one buffer only;
 * - APP_AI_SNAPSHOT_MAX_EDGE squared;
 * - active snapshot bytes-per-pixel;
 * - capacity aligned to the supplied cache-line size;
 * - zero-initialized allocation;
 * - MALLOC_CAP_SPIRAM.
 *
 * Scheduling and inference policy remain outside this service.
 */
esp_err_t vision_ai_snapshot_buffer_init(size_t alignment);

uint8_t *vision_ai_snapshot_buffer_data(void);

size_t vision_ai_snapshot_buffer_capacity(void);

/*
 * Synchronize bytes written by the producer CPU to external memory.
 *
 * This preserves the previous ESP_CACHE_MSYNC_FLAG_DIR_C2M operation,
 * including cache-line-size rounding.
 */
void vision_ai_snapshot_buffer_sync_cpu_to_memory(size_t data_size);

/*
 * Synchronize snapshot bytes from external memory to the worker CPU cache.
 *
 * This preserves the previous ESP_CACHE_MSYNC_FLAG_DIR_M2C operation,
 * including the previous data_size > 0 guard.
 */
void vision_ai_snapshot_buffer_sync_memory_to_cpu(size_t data_size);

#ifdef __cplusplus
}
#endif
