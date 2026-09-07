#include "services/vision/face_detector.h"

#include "config/app_features.h"
#include "services/vision/backends/face_detector_backend.h"

extern "C" esp_err_t face_detect_init(void)
{
#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_ESPDL
    return espdl_face_detect_init();
#elif APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_FP32
    return tflm_fp32_face_detect_init();
#elif APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_INT8
    return tflm_int8_face_detect_init();
#else
#error "Unsupported face-detection backend"
#endif
}

extern "C" int face_detect_run_rgb565(
    uint8_t *buf,
    uint32_t width,
    uint32_t height,
    face_box_t *boxes,
    int max_boxes)
{
#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_ESPDL
    return espdl_face_detect_run_rgb565(buf, width, height, boxes, max_boxes);
#elif APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_FP32
    return tflm_fp32_face_detect_run_rgb565(
        buf, width, height, boxes, max_boxes);
#elif APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_INT8
    return tflm_int8_face_detect_run_rgb565(
        buf, width, height, boxes, max_boxes);
#else
    return 0;
#endif
}

extern "C" int face_detect_run_rgb888(
    uint8_t *buf,
    uint32_t width,
    uint32_t height,
    face_box_t *boxes,
    int max_boxes)
{
#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_ESPDL
    return espdl_face_detect_run_rgb888(buf, width, height, boxes, max_boxes);
#elif APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_FP32
    return tflm_fp32_face_detect_run_rgb888(
        buf, width, height, boxes, max_boxes);
#elif APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_INT8
    return tflm_int8_face_detect_run_rgb888(
        buf, width, height, boxes, max_boxes);
#else
    return 0;
#endif
}

extern "C" const char *face_detect_backend_name(void)
{
#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_ESPDL
    return "ESP-DL";
#elif APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_FP32
    return "TFLM-FP32/BLAZEFACE";
#elif APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_INT8
    return "TFLM-INT8+ESP-NN/BLAZEFACE";
#else
    return "UNKNOWN";
#endif
}
