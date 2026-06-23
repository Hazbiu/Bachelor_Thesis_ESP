#pragma once

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline void core_trace(const char *tag, const char *stage)
{
    TaskHandle_t task = xTaskGetCurrentTaskHandle();

    ESP_LOGI(tag,
             "[CORE-PROOF] stage=%s cpu=%d task=%s affinity=%d time_us=%lld",
             stage,
             xPortGetCoreID(),                 // actual CPU executing this line
             pcTaskGetName(task),
             xTaskGetCoreID(task),             // task's configured affinity
             (long long)esp_timer_get_time());
}
