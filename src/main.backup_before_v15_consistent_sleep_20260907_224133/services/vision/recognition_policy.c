#include "services/vision/recognition_policy.h"

#include "config/app_features.h"


bool vision_recognition_policy_allows(
    float detector_score,
    float minimum_score)
{
#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_INT8
    /*
     * The INT8 detector has already applied its own candidate threshold.
     *
     * Do not filter the accepted candidate a second time before
     * MobileFaceNet. This preserves the existing behavior around the
     * quantized q=122 -> probability 0.50 detector threshold.
     */
    (void)detector_score;
    (void)minimum_score;
    return true;
#else
    /*
     * Preserve the existing recognition gate used by ESP-DL
     * and TFLM FP32.
     */
    return detector_score > minimum_score;
#endif
}


bool vision_recognition_policy_should_run_frame(
    uint32_t frame_id,
    uint32_t interval_frames)
{
#if APP_FACE_DETECT_BACKEND == APP_AI_BACKEND_TFLM_INT8
    /*
     * Preserve the existing INT8 positive path:
     * every accepted detector frame immediately enters MobileFaceNet.
     */
    (void)frame_id;
    (void)interval_frames;
    return true;
#else
    /*
     * Preserve the existing ESP-DL / TFLM FP32 recognition cadence exactly.
     */
    return (frame_id % interval_frames) == 0U;
#endif
}
