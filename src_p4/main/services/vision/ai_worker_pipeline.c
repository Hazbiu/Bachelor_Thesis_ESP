#include "services/vision/ai_worker_pipeline.h"

#include <inttypes.h>
#include <stdio.h>

#include "config/app_config.h"
#include "config/app_features.h"
#include "diagnostics/ai_pipeline_status.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "services/vision/ai_inference_guard.h"
#include "services/vision/ai_snapshot_buffer.h"
#include "services/vision/ai_worker_runtime.h"
#include "services/vision/ai_worker_state.h"
#include "services/vision/face_detection_result_processor.h"
#include "services/vision/face_detector.h"
#include "services/vision/face_recognition_result_processor.h"
#include "services/vision/face_recognizer.h"
#include "services/vision/face_result_store.h"
#include "services/vision/recognition_policy.h"


static const char *TAG = "app_main";
static vision_ai_worker_pipeline_hooks_t s_hooks;


static bool hooks_are_complete(
    const vision_ai_worker_pipeline_hooks_t *hooks)
{
    return hooks != NULL &&
        hooks->idle_scan_display_is_suspended != NULL &&
        hooks->sleep_is_requested != NULL &&
        hooks->camera_processing_blocked != NULL &&
        hooks->note_detection_result != NULL &&
        hooks->acquire_face_boost != NULL &&
        hooks->notify_activity != NULL &&
        hooks->recognition_started != NULL &&
        hooks->request_pin != NULL &&
        hooks->recognition_not_authenticated != NULL &&
        hooks->launch_pin_transition != NULL &&
        hooks->release_face_boost != NULL;
}


static void ai_worker_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "[CORE-PROOF] AI worker running: actual_cpu=%d task=%s",
             xPortGetCoreID(), pcTaskGetName(NULL));

    for (;;) {
        vision_ai_worker_runtime_wait_for_notification();

        vision_ai_worker_job_t job = {0};

        if (!vision_ai_worker_state_take_job(&job)) {
            continue;
        }

        if (s_hooks.sleep_is_requested() || s_hooks.camera_processing_blocked()) {
            vision_ai_worker_state_mark_idle();
            continue;
        }

        if (!vision_ai_inference_guard_lock()) {
            ESP_LOGE(TAG, "Could not acquire AI inference mutex");
            vision_ai_worker_state_mark_idle();
            continue;
        }

        esp_err_t boost_ret = s_hooks.acquire_face_boost();
        if (boost_ret != ESP_OK) {
            ESP_LOGW(TAG, "Could not request AI high-performance CPU lock: %s",
                     esp_err_to_name(boost_ret));
        }

        vision_ai_snapshot_buffer_sync_memory_to_cpu(job.data_size);

        face_box_t snapshot_boxes[APP_MAX_FACE_BOXES] = {0};
        diagnostics_ai_frame_sent_to_detector();

#if APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565
        const int face_count = face_detect_run_rgb565(
            vision_ai_snapshot_buffer_data(),
            job.width,
            job.height,
            snapshot_boxes,
            APP_MAX_FACE_BOXES);
#else
        const int face_count = face_detect_run_rgb888(
            vision_ai_snapshot_buffer_data(),
            job.width,
            job.height,
            snapshot_boxes,
            APP_MAX_FACE_BOXES);
#endif

        ESP_LOGD(TAG,
                 "AI worker detection complete: frame=%" PRIu32 " faces=%d",
                 job.frame_id, face_count);
        diagnostics_ai_detection_result(face_count);

        if (s_hooks.note_detection_result(face_count)) {
            ESP_LOGI(
                TAG,
                "Authentication rearmed after the face left the camera");
        }

        if (face_count <= 0) {
            if (vision_face_result_store_note_no_face()) {
                (void)s_hooks.release_face_boost("face left camera");
            }

            (void)s_hooks.release_face_boost("detector completed with no face");
            vision_ai_inference_guard_unlock();
            vision_ai_worker_state_mark_idle();
            continue;
        }

        s_hooks.notify_activity();

#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_INT8 && APP_POWER_VERBOSE_AI_TRACE
        printf(
            "[INT8-POSITIVE-PATH] frame=%" PRIu32
            " faces=%d idle_scan=%d display_suspended=%d\n",
            job.frame_id,
            face_count,
            job.idle_scan_frame ? 1 : 0,
            s_hooks.idle_scan_display_is_suspended() ? 1 : 0);
#endif

        const int update_count =
            vision_face_detection_result_process_and_publish(
                snapshot_boxes,
                face_count,
                job.width,
                job.height,
                job.source_width,
                job.source_height);

        if (job.idle_scan_frame ||
            s_hooks.idle_scan_display_is_suspended()) {
            ESP_LOGI(
                TAG,
                "IDLE-SCAN face detected; recognition deferred until display restore");
            (void)s_hooks.release_face_boost(
                "IDLE-SCAN detector completed");
            vision_ai_inference_guard_unlock();
            vision_ai_worker_state_mark_idle();
            continue;
        }

        const bool run_recognition =
            vision_recognition_policy_should_run_frame(
                job.frame_id,
                APP_FACE_RECOG_INTERVAL_FRAMES);

        bool pin_transition_requested = false;

        if (run_recognition) {
            for (int i = 0; i < update_count; i++) {
                if (!vision_recognition_policy_allows(
                        snapshot_boxes[i].score,
                        APP_FACE_RECOG_MIN_SCORE)) {
                    continue;
                }

                char name[FACE_RECOG_MAX_NAME_LEN] = "unknown";
                float recog_score = 0.0f;

#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_INT8 && APP_POWER_VERBOSE_AI_TRACE
                printf(
                    "[INT8-RECOG-TRIGGER] frame=%" PRIu32
                    " face=%d detector_score=%.6f\n",
                    job.frame_id,
                    i,
                    (double)snapshot_boxes[i].score);
#endif

                s_hooks.recognition_started();

                esp_err_t recog_ret = face_recognition_recognize(
                    vision_ai_snapshot_buffer_data(),
                    job.width,
                    job.height,
                    &snapshot_boxes[i],
                    name,
                    sizeof(name),
                    &recog_score);

                diagnostics_ai_recognition_result(
                    recog_ret,
                    name,
                    recog_score);

                vision_face_recognition_result_normalize_and_publish(
                    i,
                    recog_ret,
                    name,
                    sizeof(name),
                    &recog_score);

                if (recog_ret == ESP_OK && s_hooks.request_pin(name)) {
                    pin_transition_requested = true;
                } else {
                    s_hooks.recognition_not_authenticated();
                }

                ESP_LOGD(TAG,
                         "Face %d recognition: name=%s similarity=%.3f result=%s",
                         i,
                         name,
                         recog_score,
                         esp_err_to_name(recog_ret));

                if (pin_transition_requested) {
                    break;
                }
            }
        }

        if (!pin_transition_requested) {
            (void)s_hooks.release_face_boost(
                run_recognition
                    ? "detector/recognizer chain completed without PIN transition"
                    : "detector completed without recognition");
        }

        vision_ai_inference_guard_unlock();
        vision_ai_worker_state_mark_idle();

        if (pin_transition_requested) {
            (void)s_hooks.launch_pin_transition();
        }
    }
}


bool vision_ai_worker_pipeline_start(
    const vision_ai_worker_pipeline_hooks_t *hooks)
{
    if (!hooks_are_complete(hooks)) {
        return false;
    }

    s_hooks = *hooks;

    return vision_ai_worker_runtime_start(
        ai_worker_task,
        "ai_worker",
        APP_AI_WORKER_STACK_SIZE,
        APP_AI_WORKER_PRIORITY,
        APP_AI_WORKER_CORE);
}
