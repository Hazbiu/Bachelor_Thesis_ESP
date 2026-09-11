#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Own only the FreeRTOS task handle and notification plumbing for the
 * asynchronous AI worker.
 *
 * The worker function itself, inference sequence, snapshot buffer, AI policy,
 * CPU-frequency policy, authentication and sleep behavior remain outside.
 */
bool vision_ai_worker_runtime_start(
    TaskFunction_t task_function,
    const char *task_name,
    uint32_t stack_size,
    UBaseType_t priority,
    BaseType_t core_id);

/* Equivalent to the previous ai_worker_task_handle != NULL readiness check. */
bool vision_ai_worker_runtime_is_started(void);

/* Notify the existing worker exactly as xTaskNotifyGive(handle) did before. */
void vision_ai_worker_runtime_notify(void);

/*
 * Wait for one worker notification using the exact previous semantics:
 * clear the notification count on exit and wait indefinitely.
 */
void vision_ai_worker_runtime_wait_for_notification(void);

#ifdef __cplusplus
}
#endif
