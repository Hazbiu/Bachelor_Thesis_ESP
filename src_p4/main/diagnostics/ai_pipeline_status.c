#include "diagnostics/ai_pipeline_status.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "config/app_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint32_t frames_to_detector = 0;
static bool detection_success = false;
static bool recognition_success = false;
static bool user_found = false;
static float live_similarity = 0.0f;

/*
 * CPU1 publishes model timings while CPU0 renders the camera preview.
 * Keep the pair coherent with a tiny SMP critical section.
 */
static portMUX_TYPE s_live_metrics_lock = portMUX_INITIALIZER_UNLOCKED;
static diagnostics_ai_live_metrics_t s_live_metrics = {0};

static uint32_t clamp_inference_us(uint64_t inference_us)
{
    return inference_us > UINT32_MAX
        ? UINT32_MAX
        : (uint32_t)inference_us;
}

void diagnostics_ai_live_metrics_reset(void)
{
    portENTER_CRITICAL(&s_live_metrics_lock);
    memset(&s_live_metrics, 0, sizeof(s_live_metrics));
    portEXIT_CRITICAL(&s_live_metrics_lock);
}

void diagnostics_ai_live_metrics_record_detection(uint64_t inference_us)
{
    portENTER_CRITICAL(&s_live_metrics_lock);
    s_live_metrics.detector_inference_us = clamp_inference_us(inference_us);
    s_live_metrics.detector_valid = true;
    portEXIT_CRITICAL(&s_live_metrics_lock);
}

void diagnostics_ai_live_metrics_record_recognition(uint64_t inference_us)
{
    portENTER_CRITICAL(&s_live_metrics_lock);
    s_live_metrics.recognizer_inference_us = clamp_inference_us(inference_us);
    s_live_metrics.recognizer_valid = true;
    portEXIT_CRITICAL(&s_live_metrics_lock);
}

void diagnostics_ai_live_metrics_snapshot(
    diagnostics_ai_live_metrics_t *out_metrics)
{
    if (out_metrics == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_live_metrics_lock);
    *out_metrics = s_live_metrics;
    portEXIT_CRITICAL(&s_live_metrics_lock);
}

void diagnostics_ai_frame_sent_to_detector(void)
{
    frames_to_detector++;
}

void diagnostics_ai_detection_result(int face_count)
{
    if (face_count > 0) {
        detection_success = true;
    }
}

void diagnostics_ai_recognition_result(
    esp_err_t ret,
    const char *name,
    float similarity)
{
    recognition_success = (ret == ESP_OK);
    live_similarity = similarity;

    if (ret == ESP_OK &&
        name != NULL &&
        strcmp(name, "unknown") != 0 &&
        similarity > 0.0f) {
        user_found = true;
    }
}

static void ai_pipeline_monitor_task(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint32_t fps = frames_to_detector;

        printf("\nAI_Face_Rec_Pipeline:\n");
        printf("\tStatus: %s\n", fps > 0 ? "Active" : "Paused");
        printf("Flow:\n");

        printf("Camera_Frames sent to AI_Face_Dec = %s\n",
               fps > 0 ? "Successful" : "Not Successful");
        printf("\tFrames Per Second = %" PRIu32 "\n", fps);

        printf("AI_Face_Dec to AI_Face_Rec = %s\n",
               recognition_success ? "Successful" : "Not Successful");

        if (user_found) {
            printf("AI_Face_Rec_Match = User Found = Keti\n");
            printf("\tLive_Similarity = %.2f\n", live_similarity);
        } else {
            printf("AI_Face_Rec_Match = User Not Found\n");
        }

        frames_to_detector = 0;
        detection_success = false;
        recognition_success = false;
        user_found = false;
        live_similarity = 0.0f;
    }
}

void diagnostics_start_ai_pipeline_monitor(void)
{
    xTaskCreatePinnedToCore(
        ai_pipeline_monitor_task,
        "ai_pipe_mon",
        4096,
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL,
        APP_SYSTEM_WORKER_CORE
    );
}
