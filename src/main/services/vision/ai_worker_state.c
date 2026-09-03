#include "services/vision/ai_worker_state.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE s_worker_state_lock =
    portMUX_INITIALIZER_UNLOCKED;

static bool s_job_pending = false;
static bool s_worker_busy = false;
static bool s_worker_accepting = false;

static vision_ai_worker_job_t s_pending_job = {0};


bool vision_ai_worker_state_try_reserve(void)
{
    bool reserved = false;

    portENTER_CRITICAL(&s_worker_state_lock);

    if (s_worker_accepting &&
        !s_worker_busy &&
        !s_job_pending) {

        /*
         * Reserve immediately before the camera frame is copied.
         *
         * This is intentionally the same behavior as the previous
         * app_main implementation.
         */
        s_job_pending = true;
        reserved = true;
    }

    portEXIT_CRITICAL(&s_worker_state_lock);

    return reserved;
}


void vision_ai_worker_state_cancel_reservation(void)
{
    portENTER_CRITICAL(&s_worker_state_lock);
    s_job_pending = false;
    portEXIT_CRITICAL(&s_worker_state_lock);
}


void vision_ai_worker_state_commit_job(
    const vision_ai_worker_job_t *job)
{
    if (job == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_worker_state_lock);

    /*
     * Do NOT set s_job_pending=true here.
     *
     * The old implementation reserved the pending flag BEFORE doing
     * the snapshot copy. Deep-sleep preparation could clear that flag
     * while copying was in progress. The later metadata write did not
     * restore it.
     *
     * Keep that exact behavior.
     */
    s_pending_job = *job;

    portEXIT_CRITICAL(&s_worker_state_lock);
}


bool vision_ai_worker_state_take_job(
    vision_ai_worker_job_t *out_job)
{
    if (out_job == NULL) {
        return false;
    }

    bool have_job = false;

    portENTER_CRITICAL(&s_worker_state_lock);

    if (s_job_pending) {
        *out_job = s_pending_job;

        s_job_pending = false;
        s_worker_busy = true;

        have_job = true;
    }

    portEXIT_CRITICAL(&s_worker_state_lock);

    return have_job;
}


void vision_ai_worker_state_mark_idle(void)
{
    portENTER_CRITICAL(&s_worker_state_lock);
    s_worker_busy = false;
    portEXIT_CRITICAL(&s_worker_state_lock);
}


void vision_ai_worker_state_pause_accepting_and_drop_pending(void)
{
    portENTER_CRITICAL(&s_worker_state_lock);

    s_worker_accepting = false;
    s_job_pending = false;

    portEXIT_CRITICAL(&s_worker_state_lock);
}


bool vision_ai_worker_state_pause_and_drain(
    uint32_t timeout_ms)
{
    const TickType_t start_tick = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    vision_ai_worker_state_pause_accepting_and_drop_pending();

    for (;;) {
        if (!vision_ai_worker_state_is_busy()) {
            return true;
        }

        if ((xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


void vision_ai_worker_state_resume_accepting(void)
{
    portENTER_CRITICAL(&s_worker_state_lock);
    s_worker_accepting = true;
    portEXIT_CRITICAL(&s_worker_state_lock);
}


bool vision_ai_worker_state_is_busy(void)
{
    bool busy;

    portENTER_CRITICAL(&s_worker_state_lock);
    busy = s_worker_busy;
    portEXIT_CRITICAL(&s_worker_state_lock);

    return busy;
}
