#include "services/vision/ai_snapshot_scheduler.h"

#include "esp_log.h"
#include "services/vision/ai_snapshot.h"
#include "services/vision/ai_snapshot_buffer.h"
#include "services/vision/ai_worker_runtime.h"
#include "services/vision/ai_worker_state.h"


static const char *TAG = "app_main";


bool vision_ai_snapshot_scheduler_schedule(
    const uint8_t *camera_buf,
    size_t camera_buf_len,
    uint32_t camera_width,
    uint32_t camera_height,
    uint32_t current_frame,
    bool idle_scan_frame)
{
    if (!vision_ai_worker_runtime_is_started() ||
        vision_ai_snapshot_buffer_data() == NULL ||
        camera_buf == NULL || camera_width == 0 || camera_height == 0) {
        return false;
    }

    if (!vision_ai_worker_state_try_reserve()) {
        return false;
    }

    uint32_t snapshot_width = 0;
    uint32_t snapshot_height = 0;
    vision_ai_snapshot_dimensions(
        camera_width,
        camera_height,
        &snapshot_width,
        &snapshot_height);

    size_t written_bytes = 0;
    if (!vision_ai_snapshot_copy(
            camera_buf,
            camera_buf_len,
            camera_width,
            camera_height,
            vision_ai_snapshot_buffer_data(),
            vision_ai_snapshot_buffer_capacity(),
            snapshot_width,
            snapshot_height,
            &written_bytes)) {
        vision_ai_worker_state_cancel_reservation();
        ESP_LOGE(TAG, "Could not copy camera frame into AI snapshot");
        return false;
    }

    vision_ai_snapshot_buffer_sync_cpu_to_memory(written_bytes);

    const vision_ai_worker_job_t job = {
        .frame_id = current_frame,
        .source_width = camera_width,
        .source_height = camera_height,
        .width = snapshot_width,
        .height = snapshot_height,
        .data_size = written_bytes,
        .idle_scan_frame = idle_scan_frame,
    };

    vision_ai_worker_state_commit_job(&job);

    vision_ai_worker_runtime_notify();
    return true;
}
