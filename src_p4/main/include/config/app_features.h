#pragma once

/*
 * AI backend selection
 * ====================
 *
 * APP_AI_BACKEND_ESPDL
 *   Existing Espressif ESP-DL path:
 *     HumanFaceDetect MSR+MNP S8
 *     HumanFaceRecognizer / MFN S8
 *
 * APP_AI_BACKEND_TFLM_FP32
 *   TensorFlow Lite Micro FP32 comparison path:
 *     BlazeFace front/short-range detector
 *     MobileFaceNet 192-D face embedding
 *
 * APP_AI_BACKEND_TFLM_INT8
 *   TensorFlow Lite Micro full-INT8 path:
 *     BlazeFace INT8 detector
 *     MobileFaceNet INT8 192-D face embedding
 *   The Espressif TFLM component is linked with esp-nn so supported INT8
 *   kernels use the ESP-NN optimized implementation on ESP32-P4.
 *
 * All three complete detector -> recognizer selections use the same public
 * face_detect_* / face_recognition_* API. Therefore app_main.c keeps one
 * common scheduling, mutex, CPU-frequency, authentication and sleep policy.
 */
#define APP_AI_BACKEND_ESPDL       1
#define APP_AI_BACKEND_TFLM_FP32   2
#define APP_AI_BACKEND_TFLM_INT8   3

#include "config/ai_backend_selection.h"

#ifndef APP_AI_BACKEND
#define APP_AI_BACKEND APP_AI_BACKEND_ESPDL
#endif

/*
 * Normally both stages use APP_AI_BACKEND. These two overrides are retained
 * for controlled hybrid experiments.
 */
#ifndef APP_FACE_DETECT_BACKEND
#define APP_FACE_DETECT_BACKEND APP_AI_BACKEND
#endif

#ifndef APP_FACE_RECOG_BACKEND
#define APP_FACE_RECOG_BACKEND APP_AI_BACKEND
#endif

#if APP_FACE_DETECT_BACKEND != APP_AI_BACKEND_ESPDL && \
    APP_FACE_DETECT_BACKEND != APP_AI_BACKEND_TFLM_FP32 && \
    APP_FACE_DETECT_BACKEND != APP_AI_BACKEND_TFLM_INT8
#error "Invalid APP_FACE_DETECT_BACKEND"
#endif

#if APP_FACE_RECOG_BACKEND != APP_AI_BACKEND_ESPDL && \
    APP_FACE_RECOG_BACKEND != APP_AI_BACKEND_TFLM_FP32 && \
    APP_FACE_RECOG_BACKEND != APP_AI_BACKEND_TFLM_INT8
#error "Invalid APP_FACE_RECOG_BACKEND"
#endif

/*
 * TFLM FP32 models.
 *
 * BlazeFace detector:
 *   input  [1,128,128,3] float32
 *   output [1,896,16] and [1,896,1] float32
 *
 * MobileFaceNet recognizer:
 *   input  [1,112,112,3] float32
 *   output [1,192] float32
 */
#define APP_TFLM_FP32_FACE_DETECT_MODEL_PATH "/sdcard/models/FDET32.TFL"
#define APP_TFLM_FP32_FACE_RECOG_MODEL_PATH  "/sdcard/models/FREC.TFL"

/*
 * Full-INT8 models generated from the same FP32 TFLite graphs.
 *
 * FDET8.TFL:
 *   input  [1,128,128,3] int8
 *   output [1,896,16] and [1,896,1] int8
 *
 * FREC8.TFL:
 *   input  [1,112,112,3] int8
 *   output [1,192] int8
 */
#define APP_TFLM_INT8_FACE_DETECT_MODEL_PATH "/sdcard/models/FDET8.TFL"
#define APP_TFLM_INT8_FACE_RECOG_MODEL_PATH  "/sdcard/models/FREC8.TFL"

/*
 * Tensor arenas are in PSRAM. Keep the same conservative arena budgets for
 * FP32 and INT8 so backend switching does not change the surrounding memory
 * policy. The INT8 models themselves are substantially smaller.
 */
#define APP_TFLM_DETECT_TENSOR_ARENA_BYTES (2U * 1024U * 1024U)
#define APP_TFLM_RECOG_TENSOR_ARENA_BYTES  (7U * 1024U * 1024U)

/* BlazeFace front-camera model constants shared by FP32 and INT8. */
#define APP_TFLM_BLAZEFACE_INPUT_SIZE             128
#define APP_TFLM_BLAZEFACE_NUM_ANCHORS            896
#define APP_TFLM_BLAZEFACE_NUM_COORDS             16
#define APP_TFLM_BLAZEFACE_NUM_KEYPOINTS          6
#define APP_TFLM_BLAZEFACE_SCORE_CLIP             100.0f
#define APP_TFLM_BLAZEFACE_SCORE_THRESHOLD        0.75f
#define APP_TFLM_BLAZEFACE_NMS_IOU_THRESHOLD      0.30f

/*
 * INT8 BlazeFace uses a much coarser quantized classifier-output grid than the
 * FP32 model.  The validated FDET8.TFL reports:
 *
 *     score scale      ~= 0.756562352
 *     score zero point = 122
 *
 * Therefore the two relevant representable probabilities around the original
 * 0.75 threshold are approximately:
 *
 *     q=122 -> sigmoid(0.0)      = 0.5000
 *     q=123 -> sigmoid(0.75656)  = 0.6806
 *     q=124 -> sigmoid(1.51312)  = 0.8195
 *
 * q=123 allowed enrollment to build, but live RGB565 frames still produced no
 * boxes.  Use q=122 / 0.50 as the INT8 *candidate-generation* threshold and
 * let MobileFaceNet recognition remain the second-stage identity verifier.
 *
 * ESP-DL and TFLM-FP32 do NOT use these two INT8-only constants.
 */
#define APP_TFLM_INT8_BLAZEFACE_SCORE_THRESHOLD   0.50f
#define APP_TFLM_INT8_FACE_RECOG_MIN_SCORE         0.50f

/*
 * BlazeFace returns a deliberately tight face ROI.  Expand only the rectangle
 * drawn on the live display by 20% of its width/height on every side.
 *
 * This is UI-only: detector coordinates/keypoints used by MobileFaceNet stay
 * unchanged, so recognition geometry is not distorted.
 */
#define APP_TFLM_INT8_DISPLAY_BOX_MARGIN_RATIO      0.30f
#define APP_TFLM_BLAZEFACE_X_SCALE                128.0f
#define APP_TFLM_BLAZEFACE_Y_SCALE                128.0f
#define APP_TFLM_BLAZEFACE_W_SCALE                128.0f
#define APP_TFLM_BLAZEFACE_H_SCALE                128.0f

/* MobileFaceNet contract and recognition policy shared by FP32 and INT8. */
#define APP_TFLM_MOBILEFACENET_INPUT_SIZE          112
#define APP_TFLM_MOBILEFACENET_EMBEDDING_DIM       192

/*
 * MobileFaceNet-style RGB normalization:
 *     normalized = pixel / 127.5 - 1.0
 *
 * The embedding is L2-normalized after inference and compared with cosine
 * similarity. Keep the same initial threshold for FP32 and INT8 so the first
 * runtime comparison changes only inference precision/runtime. Re-tune only
 * after collecting validation measurements.
 */
#define APP_TFLM_RECOG_SIMILARITY_THRESHOLD        0.60f

/*
 * Alignment geometry used by both TFLM recognizers:
 * crop size = inter-eye distance * 2.5
 * crop center is shifted down from the eye midpoint by 15% of crop size.
 */
#define APP_TFLM_RECOG_EYE_DISTANCE_SCALE          2.5f
#define APP_TFLM_RECOG_EYE_CENTER_DOWN_SHIFT       0.15f

/* Separate databases: never mix embeddings from different inference models. */
#define APP_TFLM_FP32_RECOG_DATABASE_PATH "/sdcard/TFLMFP32.DB"
#define APP_TFLM_INT8_RECOG_DATABASE_PATH "/sdcard/TFLMINT8.DB"

/* Existing raw RGB888 enrollment dataset. */
#define APP_FACE_ENROLL_ROOT          "/sdcard/enroll"
#define APP_FACE_ENROLL_WIDTH         320
#define APP_FACE_ENROLL_HEIGHT        240
