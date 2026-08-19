#include "services/vision/face_recognizer.h"

#include "config/app_features.h"
#include "services/vision/backends/face_recognizer_backend.h"

extern "C" esp_err_t face_recognition_init(void)
{
#if APP_FACE_RECOG_BACKEND == APP_AI_BACKEND_ESPDL
    return espdl_face_recognition_init();
#elif APP_FACE_RECOG_BACKEND == APP_AI_BACKEND_TFLM_FP32
    return tflm_fp32_face_recognition_init();
#else
#error "Unsupported face-recognition backend"
#endif
}

extern "C" esp_err_t face_recognition_recognize(
    uint8_t *camera_buf,
    uint32_t width,
    uint32_t height,
    const face_box_t *box,
    char *out_name,
    int out_name_len,
    float *out_score)
{
#if APP_FACE_RECOG_BACKEND == APP_AI_BACKEND_ESPDL
    return espdl_face_recognition_recognize(
        camera_buf, width, height, box, out_name, out_name_len, out_score);
#elif APP_FACE_RECOG_BACKEND == APP_AI_BACKEND_TFLM_FP32
    return tflm_fp32_face_recognition_recognize(
        camera_buf, width, height, box, out_name, out_name_len, out_score);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

extern "C" int face_recognition_get_count(void)
{
#if APP_FACE_RECOG_BACKEND == APP_AI_BACKEND_ESPDL
    return espdl_face_recognition_get_count();
#elif APP_FACE_RECOG_BACKEND == APP_AI_BACKEND_TFLM_FP32
    return tflm_fp32_face_recognition_get_count();
#else
    return 0;
#endif
}

extern "C" const char *face_recognition_backend_name(void)
{
#if APP_FACE_RECOG_BACKEND == APP_AI_BACKEND_ESPDL
    return "ESP-DL";
#elif APP_FACE_RECOG_BACKEND == APP_AI_BACKEND_TFLM_FP32
    return "TFLM-FP32/MOBILEFACENET";
#else
    return "UNKNOWN";
#endif
}
