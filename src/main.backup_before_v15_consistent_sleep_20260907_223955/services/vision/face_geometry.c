#include "services/vision/face_geometry.h"

#include <stddef.h>
#include <stdint.h>


void vision_face_geometry_scale_box_to_source(
    const face_box_t *snapshot_box,
    uint32_t snapshot_width,
    uint32_t snapshot_height,
    uint32_t source_width,
    uint32_t source_height,
    face_box_t *source_box)
{
    if (snapshot_box == NULL || source_box == NULL ||
        snapshot_width == 0 || snapshot_height == 0) {
        return;
    }

    *source_box = *snapshot_box;

    source_box->x1 =
        (int)((int64_t)snapshot_box->x1 * source_width / snapshot_width);

    source_box->x2 =
        (int)((int64_t)snapshot_box->x2 * source_width / snapshot_width);

    source_box->y1 =
        (int)((int64_t)snapshot_box->y1 * source_height / snapshot_height);

    source_box->y2 =
        (int)((int64_t)snapshot_box->y2 * source_height / snapshot_height);

    for (int i = 0;
         i + 1 < source_box->keypoint_count && i + 1 < 10;
         i += 2) {

        source_box->keypoints[i] =
            (int)((int64_t)snapshot_box->keypoints[i] *
                  source_width /
                  snapshot_width);

        source_box->keypoints[i + 1] =
            (int)((int64_t)snapshot_box->keypoints[i + 1] *
                  source_height /
                  snapshot_height);
    }
}

float vision_face_geometry_iou(
    const face_box_t *a,
    const face_box_t *b)
{
    const int intersection_x1 =
        a->x1 > b->x1 ? a->x1 : b->x1;

    const int intersection_y1 =
        a->y1 > b->y1 ? a->y1 : b->y1;

    const int intersection_x2 =
        a->x2 < b->x2 ? a->x2 : b->x2;

    const int intersection_y2 =
        a->y2 < b->y2 ? a->y2 : b->y2;

    const int intersection_w =
        intersection_x2 > intersection_x1
            ? intersection_x2 - intersection_x1
            : 0;

    const int intersection_h =
        intersection_y2 > intersection_y1
            ? intersection_y2 - intersection_y1
            : 0;

    const int intersection_area =
        intersection_w * intersection_h;

    const int area_a =
        (a->x2 - a->x1) *
        (a->y2 - a->y1);

    const int area_b =
        (b->x2 - b->x1) *
        (b->y2 - b->y1);

    const int union_area =
        area_a + area_b - intersection_area;

    return union_area > 0
        ? (float)intersection_area / (float)union_area
        : 0.0f;
}

int vision_face_geometry_smooth_coordinate(
    int old_value,
    int new_value)
{
    return (old_value * 3 + new_value) / 4;
}


void vision_face_geometry_expand_box_for_display(
    face_box_t *box,
    uint32_t source_width,
    uint32_t source_height,
    float margin_ratio)
{
    if (box == NULL || source_width == 0 || source_height == 0 ||
        margin_ratio <= 0.0f) {
        return;
    }

    const int width = box->x2 - box->x1;
    const int height = box->y2 - box->y1;

    if (width <= 0 || height <= 0) {
        return;
    }

    const int margin_x = (int)((float)width * margin_ratio + 0.5f);
    const int margin_y = (int)((float)height * margin_ratio + 0.5f);

    box->x1 -= margin_x;
    box->y1 -= margin_y;
    box->x2 += margin_x;
    box->y2 += margin_y;

    if (box->x1 < 0) {
        box->x1 = 0;
    }

    if (box->y1 < 0) {
        box->y1 = 0;
    }

    if (box->x2 >= (int)source_width) {
        box->x2 = (int)source_width - 1;
    }

    if (box->y2 >= (int)source_height) {
        box->y2 = (int)source_height - 1;
    }
}
