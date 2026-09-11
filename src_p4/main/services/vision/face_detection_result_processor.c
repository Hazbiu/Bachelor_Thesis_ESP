#include "services/vision/face_detection_result_processor.h"

#include "config/ai_backend_selection.h"
#include "config/app_config.h"
#include "config/app_features.h"
#include "esp_log.h"
#include "services/vision/face_geometry.h"
#include "services/vision/face_result_store.h"


static const char *TAG = "app_main";


int vision_face_detection_result_process_and_publish(
    const face_box_t *snapshot_boxes,
    int face_count,
    uint32_t snapshot_width,
    uint32_t snapshot_height,
    uint32_t source_width,
    uint32_t source_height)
{
    if (snapshot_boxes == NULL || face_count <= 0) {
        return 0;
    }

    int update_count = face_count;
    if (update_count > APP_MAX_FACE_BOXES) {
        update_count = APP_MAX_FACE_BOXES;
    }

    face_box_t source_boxes[APP_MAX_FACE_BOXES] = {0};

    for (int i = 0; i < update_count; i++) {
        vision_face_geometry_scale_box_to_source(
            &snapshot_boxes[i],
            snapshot_width,
            snapshot_height,
            source_width,
            source_height,
            &source_boxes[i]);

#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_INT8
        vision_face_geometry_expand_box_for_display(
            &source_boxes[i],
            source_width,
            source_height,
            APP_TFLM_INT8_DISPLAY_BOX_MARGIN_RATIO);
#endif

        ESP_LOGI(
            TAG,
            "Face %d score=%.2f box=[%d,%d,%d,%d]",
            i,
            snapshot_boxes[i].score,
            source_boxes[i].x1,
            source_boxes[i].y1,
            source_boxes[i].x2,
            source_boxes[i].y2);
    }

    /*
     * Publish red boxes immediately; recognition may take many seconds.
     * This is intentionally the same timing as the former app_main block.
     */
    vision_face_result_store_publish_detected_boxes(
        source_boxes,
        update_count,
        true);

    return update_count;
}
