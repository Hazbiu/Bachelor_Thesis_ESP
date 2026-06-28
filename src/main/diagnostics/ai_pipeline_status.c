#include "diagnostics/ai_pipeline_status.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint32_t frames_to_detector = 0;
static bool detection_success = false;
static bool recognition_success = false;
static bool user_found = false;
static float live_similarity = 0.0f;

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

void diagnostics_ai_recognition_result(esp_err_t ret, const char *name, float similarity)
{
    recognition_success = (ret == ESP_OK);
    live_similarity = similarity;

    if (ret == ESP_OK && name != NULL && strcmp(name, "unknown") != 0 && similarity > 0.0f) {
        user_found = true;
    }
}

static void ai_pipeline_monitor_task(void *arg)
{
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
        tskNO_AFFINITY
    );
}