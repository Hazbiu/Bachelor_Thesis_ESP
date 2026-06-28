#include "diagnostics/cpu_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define CPU_STATS_INTERVAL_MS 3000
#define TASK_ARRAY_EXTRA_SIZE 8

static const char *task_state_to_string(eTaskState state)
{
    switch (state) {
    case eRunning:   return "Running";
    case eReady:     return "Ready";
    case eBlocked:   return "Blocked";
    case eSuspended: return "Suspended";
    case eDeleted:   return "Deleted";
    case eInvalid:   return "Invalid";
    default:         return "Unknown";
    }
}

static int find_task_by_handle(TaskStatus_t *array, UBaseType_t count, TaskHandle_t handle)
{
    for (UBaseType_t i = 0; i < count; i++) {
        if (array[i].xHandle == handle) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t percent_x100(uint64_t part, uint64_t total)
{
    if (total == 0) {
        return 0;
    }
    return (uint32_t)((part * 10000ULL) / total);
}

static void print_cpu_thread_stats_once(void)
{
#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    printf("CPU stats not enabled\n");
    return;
#else
    UBaseType_t start_size = uxTaskGetNumberOfTasks() + TASK_ARRAY_EXTRA_SIZE;
    TaskStatus_t *start_array = calloc(start_size, sizeof(TaskStatus_t));
    if (!start_array) return;

    configRUN_TIME_COUNTER_TYPE start_total = 0;
    UBaseType_t start_count = uxTaskGetSystemState(start_array, start_size, &start_total);
    if (start_count == 0) {
        free(start_array);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(CPU_STATS_INTERVAL_MS));

    UBaseType_t end_size = uxTaskGetNumberOfTasks() + TASK_ARRAY_EXTRA_SIZE;
    TaskStatus_t *end_array = calloc(end_size, sizeof(TaskStatus_t));
    if (!end_array) {
        free(start_array);
        return;
    }

    configRUN_TIME_COUNTER_TYPE end_total = 0;
    UBaseType_t end_count = uxTaskGetSystemState(end_array, end_size, &end_total);
    if (end_count == 0) {
        free(start_array);
        free(end_array);
        return;
    }

    uint64_t total_delta = (uint64_t)(end_total - start_total);
    uint64_t total_capacity = total_delta * CONFIG_FREERTOS_NUMBER_OF_CORES;

    uint32_t idle0_x100 = 0;
    uint32_t idle1_x100 = 0;

    printf("\nThread Number | Thread Name | Thread State | CPU Usage\n");

    for (UBaseType_t i = 0; i < end_count; i++) {
        int start_index = find_task_by_handle(start_array, start_count, end_array[i].xHandle);

        uint64_t task_delta = 0;
        if (start_index >= 0) {
            task_delta = (uint64_t)(end_array[i].ulRunTimeCounter -
                                    start_array[start_index].ulRunTimeCounter);
        }

        uint32_t task_pct_x100 = percent_x100(task_delta, total_capacity);

        const char *name = end_array[i].pcTaskName ? end_array[i].pcTaskName : "unknown";
        const char *state = task_state_to_string(end_array[i].eCurrentState);

        printf("%u | %s | %s | %" PRIu32 ".%02" PRIu32 "%%\n",
               (unsigned int)end_array[i].xTaskNumber,
               name,
               state,
               task_pct_x100 / 100,
               task_pct_x100 % 100);

        if (strcmp(name, "IDLE0") == 0) {
            idle0_x100 = task_pct_x100;
        } else if (strcmp(name, "IDLE1") == 0) {
            idle1_x100 = task_pct_x100;
        }
    }

#if CONFIG_FREERTOS_NUMBER_OF_CORES == 2
    uint32_t cpu0_usage_x100 = 10000 - ((idle0_x100 * 2 > 10000) ? 10000 : idle0_x100 * 2);
    uint32_t cpu1_usage_x100 = 10000 - ((idle1_x100 * 2 > 10000) ? 10000 : idle1_x100 * 2);

    printf("- | CPU0_TOTAL | Total | %" PRIu32 ".%02" PRIu32 "%%\n",
           cpu0_usage_x100 / 100, cpu0_usage_x100 % 100);

    printf("- | CPU1_TOTAL | Total | %" PRIu32 ".%02" PRIu32 "%%\n",
           cpu1_usage_x100 / 100, cpu1_usage_x100 % 100);
#endif

    uint32_t total_idle_x100 = idle0_x100 + idle1_x100;
    uint32_t total_cpu_x100 = total_idle_x100 >= 10000 ? 0 : 10000 - total_idle_x100;

    printf("- | BOTH_CPUS_TOTAL | Total | %" PRIu32 ".%02" PRIu32 "%%\n",
           total_cpu_x100 / 100, total_cpu_x100 % 100);

    free(start_array);
    free(end_array);
#endif
}

static void cpu_stats_task(void *arg)
{
    while (1) {
        print_cpu_thread_stats_once();
    }
}

void diagnostics_start_cpu_stats_monitor(void)
{
    xTaskCreatePinnedToCore(
        cpu_stats_task,
        "cpu_stats_dbg",
        8192,
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL,
        tskNO_AFFINITY
    );
}