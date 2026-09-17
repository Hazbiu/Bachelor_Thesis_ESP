#include "diagnostics/cpu_stats.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/app_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define CPU_STATS_INTERVAL_MS             3000U
#define CPU_LIVE_USAGE_SAMPLE_INTERVAL_MS 1000U
#define CPU_LIVE_USAGE_TASK_STACK_SIZE    3072U
#define TASK_ARRAY_EXTRA_SIZE             8U

/*
 * Live HP Core 0 / Core 1 telemetry.
 *
 * ESP-IDF documents that ulTaskGetIdleRunTimeCounter() queries the idle task
 * of the CURRENT core when configNUMBER_OF_CORES > 1.  Therefore two tiny
 * samplers are used, each permanently pinned to the HP core it measures.
 *
 * With CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER enabled by the installer,
 * the IDLE run-time counter is in microseconds.  The denominator is the elapsed
 * FreeRTOS tick interval.  The resulting percentage is:
 *
 *     core usage = 100% - core idle percentage
 *
 * The samplers only cache two uint32_t values.  The live camera path never
 * calls uxTaskGetSystemState(), so it does not suspend the scheduler to render
 * the on-screen percentages.
 */
static portMUX_TYPE s_live_usage_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_live_usage_task_handle[2] = {NULL, NULL};
static bool s_live_usage_reset_requested[2] = {true, true};
static bool s_live_usage_value_valid[2] = {false, false};
static uint32_t s_live_usage_x10[2] = {0U, 0U};

void diagnostics_cpu_hp_usage_reset(void)
{
    portENTER_CRITICAL(&s_live_usage_lock);

    for (int core_id = 0; core_id < 2; ++core_id) {
        s_live_usage_reset_requested[core_id] = true;
        s_live_usage_value_valid[core_id] = false;
        s_live_usage_x10[core_id] = 0U;
    }

    portEXIT_CRITICAL(&s_live_usage_lock);
}

bool diagnostics_cpu_hp_usage_snapshot(
    uint32_t *core0_usage_x10,
    uint32_t *core1_usage_x10)
{
    if (core0_usage_x10 == NULL || core1_usage_x10 == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_live_usage_lock);

    const bool valid =
        s_live_usage_value_valid[0] &&
        s_live_usage_value_valid[1];

    const uint32_t core0_value = s_live_usage_x10[0];
    const uint32_t core1_value = s_live_usage_x10[1];

    portEXIT_CRITICAL(&s_live_usage_lock);

    *core0_usage_x10 = core0_value;
    *core1_usage_x10 = core1_value;

    return valid;
}

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && \
    CONFIG_FREERTOS_NUMBER_OF_CORES > 1

static void cpu_hp_core_live_usage_task(void *arg)
{
    const int core_id = (int)(intptr_t)arg;

    printf(
        "[CPU-LIVE] sampler_started core=%d target_core=%d interval_ms=%u\n",
        xPortGetCoreID(),
        core_id,
        (unsigned)CPU_LIVE_USAGE_SAMPLE_INTERVAL_MS);

    /*
     * Because this task is pinned, ulTaskGetIdleRunTimeCounter() always reads
     * the idle task of the intended HP core.
     */
    TickType_t previous_tick = xTaskGetTickCount();
    configRUN_TIME_COUNTER_TYPE previous_idle_runtime =
        ulTaskGetIdleRunTimeCounter();

    TickType_t wake_tick = previous_tick;

    for (;;) {
        vTaskDelayUntil(
            &wake_tick,
            pdMS_TO_TICKS(CPU_LIVE_USAGE_SAMPLE_INTERVAL_MS));

        const TickType_t current_tick = xTaskGetTickCount();
        const configRUN_TIME_COUNTER_TYPE current_idle_runtime =
            ulTaskGetIdleRunTimeCounter();

        bool reset_requested;

        portENTER_CRITICAL(&s_live_usage_lock);

        reset_requested =
            s_live_usage_reset_requested[core_id];

        if (reset_requested) {
            s_live_usage_reset_requested[core_id] = false;
            s_live_usage_value_valid[core_id] = false;
            s_live_usage_x10[core_id] = 0U;
        }

        portEXIT_CRITICAL(&s_live_usage_lock);

        if (reset_requested) {
            previous_tick = current_tick;
            previous_idle_runtime = current_idle_runtime;
            continue;
        }

        /*
         * Native unsigned subtraction handles normal tick/run-time counter wrap.
         */
        const TickType_t elapsed_ticks =
            current_tick - previous_tick;

        const configRUN_TIME_COUNTER_TYPE idle_delta_native =
            current_idle_runtime - previous_idle_runtime;

        previous_tick = current_tick;
        previous_idle_runtime = current_idle_runtime;

        if (elapsed_ticks == 0) {
            continue;
        }

        const uint64_t elapsed_us =
            (uint64_t)elapsed_ticks *
            (uint64_t)portTICK_PERIOD_MS *
            1000ULL;

        uint64_t idle_us =
            (uint64_t)idle_delta_native;

        /*
         * Run-time accounting and RTOS tick sampling are not simultaneous.
         * Clamp the tiny edge discrepancy instead of allowing negative busy
         * time if an idle sample lands a few microseconds past the tick sample.
         */
        if (idle_us > elapsed_us) {
            idle_us = elapsed_us;
        }

        const uint64_t busy_us =
            elapsed_us - idle_us;

        uint64_t usage_x10 =
            (busy_us * 1000ULL + elapsed_us / 2ULL) /
            elapsed_us;

        if (usage_x10 > 1000ULL) {
            usage_x10 = 1000ULL;
        }

        portENTER_CRITICAL(&s_live_usage_lock);

        s_live_usage_x10[core_id] = (uint32_t)usage_x10;
        s_live_usage_value_valid[core_id] = true;

        portEXIT_CRITICAL(&s_live_usage_lock);
    }
}

#endif

esp_err_t diagnostics_cpu_hp_usage_start(void)
{
#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS || \
    CONFIG_FREERTOS_NUMBER_OF_CORES < 2

    /*
     * The installer enables the required Kconfig options before building.
     * Keeping this runtime check makes a manual source-only copy fail safely.
     */
    return ESP_ERR_NOT_SUPPORTED;
#else
    /*
     * Normal application flow calls this once.  If called again after both
     * samplers exist, simply reset the measurement windows.
     */
    portENTER_CRITICAL(&s_live_usage_lock);

    const bool both_started =
        s_live_usage_task_handle[0] != NULL &&
        s_live_usage_task_handle[1] != NULL;

    portEXIT_CRITICAL(&s_live_usage_lock);

    if (both_started) {
        diagnostics_cpu_hp_usage_reset();
        return ESP_OK;
    }

    for (int core_id = 0; core_id < 2; ++core_id) {
        portENTER_CRITICAL(&s_live_usage_lock);
        const bool already_started =
            s_live_usage_task_handle[core_id] != NULL;
        portEXIT_CRITICAL(&s_live_usage_lock);

        if (already_started) {
            continue;
        }

        TaskHandle_t new_task = NULL;

        const BaseType_t created =
            xTaskCreatePinnedToCore(
                cpu_hp_core_live_usage_task,
                core_id == 0
                    ? "cpu0_live_usage"
                    : "cpu1_live_usage",
                CPU_LIVE_USAGE_TASK_STACK_SIZE,
                (void *)(intptr_t)core_id,
                tskIDLE_PRIORITY + 1,
                &new_task,
                core_id);

        if (created != pdPASS || new_task == NULL) {
            return ESP_ERR_NO_MEM;
        }

        portENTER_CRITICAL(&s_live_usage_lock);
        s_live_usage_task_handle[core_id] = new_task;
        s_live_usage_reset_requested[core_id] = true;
        s_live_usage_value_valid[core_id] = false;
        s_live_usage_x10[core_id] = 0U;
        portEXIT_CRITICAL(&s_live_usage_lock);
    }

    return ESP_OK;
#endif
}


/*
 * Existing verbose task-table diagnostics.
 *
 * This is intentionally retained for manual debugging.  The camera overlay
 * does not call this path because uxTaskGetSystemState() suspends the scheduler
 * while collecting every task record.
 */
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

static int find_task_by_handle(
    TaskStatus_t *array,
    UBaseType_t count,
    TaskHandle_t handle)
{
    for (UBaseType_t i = 0; i < count; i++) {
        if (array[i].xHandle == handle) {
            return (int)i;
        }
    }

    return -1;
}

static uint32_t percent_x100(
    uint64_t part,
    uint64_t total)
{
    if (total == 0) {
        return 0;
    }

    return (uint32_t)(
        (part * 10000ULL) / total);
}

static void print_cpu_thread_stats_once(void)
{
#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    printf("CPU stats not enabled\n");
    vTaskDelay(pdMS_TO_TICKS(CPU_STATS_INTERVAL_MS));
    return;
#else
    UBaseType_t start_size =
        uxTaskGetNumberOfTasks() +
        TASK_ARRAY_EXTRA_SIZE;

    TaskStatus_t *start_array =
        calloc(
            start_size,
            sizeof(TaskStatus_t));

    if (!start_array) {
        vTaskDelay(
            pdMS_TO_TICKS(
                CPU_STATS_INTERVAL_MS));
        return;
    }

    configRUN_TIME_COUNTER_TYPE start_total = 0;

    UBaseType_t start_count =
        uxTaskGetSystemState(
            start_array,
            start_size,
            &start_total);

    if (start_count == 0) {
        free(start_array);
        vTaskDelay(
            pdMS_TO_TICKS(
                CPU_STATS_INTERVAL_MS));
        return;
    }

    vTaskDelay(
        pdMS_TO_TICKS(
            CPU_STATS_INTERVAL_MS));

    UBaseType_t end_size =
        uxTaskGetNumberOfTasks() +
        TASK_ARRAY_EXTRA_SIZE;

    TaskStatus_t *end_array =
        calloc(
            end_size,
            sizeof(TaskStatus_t));

    if (!end_array) {
        free(start_array);
        return;
    }

    configRUN_TIME_COUNTER_TYPE end_total = 0;

    UBaseType_t end_count =
        uxTaskGetSystemState(
            end_array,
            end_size,
            &end_total);

    if (end_count == 0) {
        free(start_array);
        free(end_array);
        return;
    }

    uint64_t total_delta =
        (uint64_t)(
            end_total - start_total);

    uint64_t total_capacity =
        total_delta *
        CONFIG_FREERTOS_NUMBER_OF_CORES;

    uint32_t idle0_x100 = 0;
    uint32_t idle1_x100 = 0;

    printf(
        "\n+---------------+------------------------+---------------+------------+\n");

    printf(
        "| %-13s | %-22s | %-13s | %-10s |\n",
        "Thread Number",
        "Thread Name",
        "Thread State",
        "CPU Usage");

    printf(
        "+---------------+------------------------+---------------+------------+\n");

    for (UBaseType_t i = 0; i < end_count; i++) {
        int start_index =
            find_task_by_handle(
                start_array,
                start_count,
                end_array[i].xHandle);

        uint64_t task_delta = 0;

        if (start_index >= 0) {
            task_delta =
                (uint64_t)(
                    end_array[i].ulRunTimeCounter -
                    start_array[start_index].ulRunTimeCounter);
        }

        uint32_t task_pct_x100 =
            percent_x100(
                task_delta,
                total_capacity);

        const char *name =
            end_array[i].pcTaskName
                ? end_array[i].pcTaskName
                : "unknown";

        const char *state =
            task_state_to_string(
                end_array[i].eCurrentState);

        printf(
            "| %-13u | %-22.22s | %-13s | %6" PRIu32 ".%02" PRIu32 "%% |\n",
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

    printf(
        "+---------------+------------------------+---------------+------------+\n");

#if CONFIG_FREERTOS_NUMBER_OF_CORES == 2
    uint32_t cpu0_usage_x100 =
        10000 -
        ((idle0_x100 * 2 > 10000)
            ? 10000
            : idle0_x100 * 2);

    uint32_t cpu1_usage_x100 =
        10000 -
        ((idle1_x100 * 2 > 10000)
            ? 10000
            : idle1_x100 * 2);

    printf(
        "| %-13s | %-22s | %-13s | %6" PRIu32 ".%02" PRIu32 "%% |\n",
        "-",
        "CPU0_TOTAL",
        "Total",
        cpu0_usage_x100 / 100,
        cpu0_usage_x100 % 100);

    printf(
        "| %-13s | %-22s | %-13s | %6" PRIu32 ".%02" PRIu32 "%% |\n",
        "-",
        "CPU1_TOTAL",
        "Total",
        cpu1_usage_x100 / 100,
        cpu1_usage_x100 % 100);
#endif

    uint32_t total_idle_x100 =
        idle0_x100 + idle1_x100;

    uint32_t total_cpu_x100 =
        total_idle_x100 >= 10000
            ? 0
            : 10000 - total_idle_x100;

    printf(
        "| %-13s | %-22s | %-13s | %6" PRIu32 ".%02" PRIu32 "%% |\n",
        "-",
        "BOTH_CPUS_TOTAL",
        "Total",
        total_cpu_x100 / 100,
        total_cpu_x100 % 100);

    printf(
        "+---------------+------------------------+---------------+------------+\n");

    free(start_array);
    free(end_array);
#endif
}

static void cpu_stats_task(void *arg)
{
    (void)arg;

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
        APP_SYSTEM_WORKER_CORE);
}
