#include "services/vision/ai_worker_runtime.h"

static TaskHandle_t s_worker_task_handle = NULL;


bool vision_ai_worker_runtime_start(
    TaskFunction_t task_function,
    const char *task_name,
    uint32_t stack_size,
    UBaseType_t priority,
    BaseType_t core_id)
{
    if (task_function == NULL || task_name == NULL) {
        return false;
    }

    if (s_worker_task_handle != NULL) {
        return true;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        task_function,
        task_name,
        stack_size,
        NULL,
        priority,
        &s_worker_task_handle,
        core_id);

    return created == pdPASS;
}


bool vision_ai_worker_runtime_is_started(void)
{
    return s_worker_task_handle != NULL;
}


void vision_ai_worker_runtime_notify(void)
{
    if (s_worker_task_handle != NULL) {
        xTaskNotifyGive(s_worker_task_handle);
    }
}

void vision_ai_worker_runtime_wait_for_notification(void)
{
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

