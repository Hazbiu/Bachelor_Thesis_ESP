#include "services/vision/ai_inference_guard.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_inference_mutex = NULL;


esp_err_t vision_ai_inference_guard_init(void)
{
    if (s_inference_mutex != NULL) {
        return ESP_OK;
    }

    s_inference_mutex = xSemaphoreCreateMutex();

    return s_inference_mutex != NULL
        ? ESP_OK
        : ESP_ERR_NO_MEM;
}


bool vision_ai_inference_guard_lock(void)
{
    if (s_inference_mutex == NULL) {
        return false;
    }

    return xSemaphoreTake(
               s_inference_mutex,
               portMAX_DELAY) == pdTRUE;
}


void vision_ai_inference_guard_unlock(void)
{
    if (s_inference_mutex != NULL) {
        (void)xSemaphoreGive(s_inference_mutex);
    }
}
