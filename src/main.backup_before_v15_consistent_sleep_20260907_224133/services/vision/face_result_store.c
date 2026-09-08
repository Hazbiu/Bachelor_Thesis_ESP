#include "services/vision/face_result_store.h"

#include <stdio.h>
#include <string.h>

#include "config/app_config.h"
#include "freertos/FreeRTOS.h"
#include "services/vision/face_geometry.h"

static face_box_t s_last_boxes[APP_MAX_FACE_BOXES];
static char s_last_face_names
    [APP_MAX_FACE_BOXES][FACE_RECOG_MAX_NAME_LEN];
static float s_last_recognition_scores[APP_MAX_FACE_BOXES];

static int s_last_face_count = 0;
static int s_no_face_frames = 0;

static portMUX_TYPE s_result_lock = portMUX_INITIALIZER_UNLOCKED;


void vision_face_result_store_clear(void)
{
    portENTER_CRITICAL(&s_result_lock);

    s_last_face_count = 0;
    s_no_face_frames = 0;

    memset(s_last_boxes, 0, sizeof(s_last_boxes));
    memset(s_last_face_names, 0, sizeof(s_last_face_names));
    memset(
        s_last_recognition_scores,
        0,
        sizeof(s_last_recognition_scores));

    portEXIT_CRITICAL(&s_result_lock);
}


void vision_face_result_store_publish_detected_boxes(
    const face_box_t *boxes,
    int count,
    bool preserve_previous_names)
{
    if (count < 0) {
        count = 0;
    }

    if (count > APP_MAX_FACE_BOXES) {
        count = APP_MAX_FACE_BOXES;
    }

    face_box_t previous_boxes[APP_MAX_FACE_BOXES] = {0};

    char previous_names
        [APP_MAX_FACE_BOXES][FACE_RECOG_MAX_NAME_LEN] = {{0}};

    float previous_scores[APP_MAX_FACE_BOXES] = {0};

    int previous_count = 0;

    /*
     * Preserve the exact old two-phase locking model:
     * first snapshot the previous result, then perform matching outside
     * the critical section, then atomically publish the new result.
     */
    portENTER_CRITICAL(&s_result_lock);

    previous_count = s_last_face_count;

    if (previous_count > APP_MAX_FACE_BOXES) {
        previous_count = APP_MAX_FACE_BOXES;
    }

    memcpy(
        previous_boxes,
        s_last_boxes,
        sizeof(previous_boxes));

    memcpy(
        previous_names,
        s_last_face_names,
        sizeof(previous_names));

    memcpy(
        previous_scores,
        s_last_recognition_scores,
        sizeof(previous_scores));

    portEXIT_CRITICAL(&s_result_lock);

    face_box_t updated_boxes[APP_MAX_FACE_BOXES] = {0};

    char updated_names
        [APP_MAX_FACE_BOXES][FACE_RECOG_MAX_NAME_LEN] = {{0}};

    float updated_scores[APP_MAX_FACE_BOXES] = {0};

    bool previous_used[APP_MAX_FACE_BOXES] = {false};

    for (int i = 0; i < count; i++) {
        int matched_previous = -1;
        float best_iou = 0.20f;

        for (int previous = 0;
             previous < previous_count;
             previous++) {

            if (previous_used[previous]) {
                continue;
            }

            const float iou =
                vision_face_geometry_iou(
                    &boxes[i],
                    &previous_boxes[previous]);

            if (iou > best_iou) {
                best_iou = iou;
                matched_previous = previous;
            }
        }

        updated_boxes[i] = boxes[i];

        if (matched_previous >= 0) {
            previous_used[matched_previous] = true;

            updated_boxes[i].x1 =
                vision_face_geometry_smooth_coordinate(
                    previous_boxes[matched_previous].x1,
                    boxes[i].x1);

            updated_boxes[i].y1 =
                vision_face_geometry_smooth_coordinate(
                    previous_boxes[matched_previous].y1,
                    boxes[i].y1);

            updated_boxes[i].x2 =
                vision_face_geometry_smooth_coordinate(
                    previous_boxes[matched_previous].x2,
                    boxes[i].x2);

            updated_boxes[i].y2 =
                vision_face_geometry_smooth_coordinate(
                    previous_boxes[matched_previous].y2,
                    boxes[i].y2);

            if (preserve_previous_names) {
                snprintf(
                    updated_names[i],
                    sizeof(updated_names[i]),
                    "%s",
                    previous_names[matched_previous]);

                updated_scores[i] =
                    previous_scores[matched_previous];
            }
        }
    }

    portENTER_CRITICAL(&s_result_lock);

    memcpy(
        s_last_boxes,
        updated_boxes,
        sizeof(updated_boxes));

    memcpy(
        s_last_face_names,
        updated_names,
        sizeof(updated_names));

    memcpy(
        s_last_recognition_scores,
        updated_scores,
        sizeof(updated_scores));

    s_last_face_count = count;
    s_no_face_frames = 0;

    portEXIT_CRITICAL(&s_result_lock);
}


void vision_face_result_store_publish_recognition_result(
    int index,
    const char *name,
    float score)
{
    if (index < 0 ||
        index >= APP_MAX_FACE_BOXES ||
        name == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_result_lock);

    if (index < s_last_face_count) {
        snprintf(
            s_last_face_names[index],
            sizeof(s_last_face_names[index]),
            "%s",
            name);

        s_last_recognition_scores[index] = score;
    }

    portEXIT_CRITICAL(&s_result_lock);
}


bool vision_face_result_store_note_no_face(void)
{
    bool hold_expired = false;

    portENTER_CRITICAL(&s_result_lock);

    s_no_face_frames++;

    if (s_no_face_frames >= APP_FACE_BOX_HOLD_MISSES) {
        s_last_face_count = 0;

        memset(
            s_last_boxes,
            0,
            sizeof(s_last_boxes));

        memset(
            s_last_face_names,
            0,
            sizeof(s_last_face_names));

        memset(
            s_last_recognition_scores,
            0,
            sizeof(s_last_recognition_scores));

        /*
         * Deliberately do not reset s_no_face_frames here.
         * The old app_main implementation behaved exactly this way.
         */
        hold_expired = true;
    }

    portEXIT_CRITICAL(&s_result_lock);

    return hold_expired;
}


int vision_face_result_store_snapshot(
    face_box_t *boxes,
    char names[][FACE_RECOG_MAX_NAME_LEN],
    float *scores,
    int capacity)
{
    if (capacity < 0) {
        capacity = 0;
    }

    if (capacity > APP_MAX_FACE_BOXES) {
        capacity = APP_MAX_FACE_BOXES;
    }

    portENTER_CRITICAL(&s_result_lock);

    int count = s_last_face_count;

    if (count > capacity) {
        count = capacity;
    }

    if (count < 0) {
        count = 0;
    }

    if (boxes != NULL && count > 0) {
        memcpy(
            boxes,
            s_last_boxes,
            sizeof(face_box_t) * (size_t)count);
    }

    if (names != NULL && count > 0) {
        memcpy(
            names,
            s_last_face_names,
            sizeof(s_last_face_names[0]) * (size_t)count);
    }

    if (scores != NULL && count > 0) {
        memcpy(
            scores,
            s_last_recognition_scores,
            sizeof(float) * (size_t)count);
    }

    portEXIT_CRITICAL(&s_result_lock);

    return count;
}
