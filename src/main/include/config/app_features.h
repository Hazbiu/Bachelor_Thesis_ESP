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
 * IMPORTANT:
 * The two downloaded models are FLOAT32. Therefore backend 2 is named
 * TFLM-FP32, not ESP-NN/INT8. This keeps the benchmark technically honest.
 *
 * The selection header is intentionally tiny so the helper scripts can switch
 * the complete detector + recognizer backend without modifying this file.
 */
#define APP_AI_BACKEND_ESPDL       1
#define APP_AI_BACKEND_TFLM_FP32   2

#include "config/ai_backend_selection.h"

#ifndef APP_AI_BACKEND
#define APP_AI_BACKEND APP_AI_BACKEND_ESPDL
#endif

/*
 * Normally both stages use APP_AI_BACKEND. These two overrides are retained
 * for controlled hybrid experiments, e.g. ESP-DL detector + TFLM recognizer.
 */
#ifndef APP_FACE_DETECT_BACKEND
#define APP_FACE_DETECT_BACKEND APP_AI_BACKEND
#endif

#ifndef APP_FACE_RECOG_BACKEND
#define APP_FACE_RECOG_BACKEND APP_AI_BACKEND
#endif

#if APP_FACE_DETECT_BACKEND != APP_AI_BACKEND_ESPDL && \
    APP_FACE_DETECT_BACKEND != APP_AI_BACKEND_TFLM_FP32
#error "Invalid APP_FACE_DETECT_BACKEND"
#endif

#if APP_FACE_RECOG_BACKEND != APP_AI_BACKEND_ESPDL && \
    APP_FACE_RECOG_BACKEND != APP_AI_BACKEND_TFLM_FP32
#error "Invalid APP_FACE_RECOG_BACKEND"
#endif

/*
 * TFLM FP32 models confirmed from the user's tensor inspection:
 *
 * BlazeFace detector
 *   input  : [1,128,128,3] float32
 *   output0: [1,896,16]    float32  regressors
 *   output1: [1,896,1]     float32  classification logits
 *
 * MobileFaceNet recognizer
 *   input  : [1,112,112,3] float32
 *   output0: [1,192]       float32  embedding
 */
#define APP_TFLM_FACE_DETECT_MODEL_PATH "/sdcard/models/FDET32.TFL"
#define APP_TFLM_FACE_RECOG_MODEL_PATH  "/sdcard/models/FREC.TFL"

/*
 * Tensor arenas are in PSRAM. These are intentionally conservative defaults.
 * The TFLM runner reports arena_used_bytes() when AI logging is enabled.
 */
#define APP_TFLM_DETECT_TENSOR_ARENA_BYTES (2U * 1024U * 1024U)
#define APP_TFLM_RECOG_TENSOR_ARENA_BYTES  (7U * 1024U * 1024U)

/* BlazeFace front-camera model constants. */
#define APP_TFLM_BLAZEFACE_INPUT_SIZE             128
#define APP_TFLM_BLAZEFACE_NUM_ANCHORS            896
#define APP_TFLM_BLAZEFACE_NUM_COORDS             16
#define APP_TFLM_BLAZEFACE_NUM_KEYPOINTS          6
#define APP_TFLM_BLAZEFACE_SCORE_CLIP             100.0f
#define APP_TFLM_BLAZEFACE_SCORE_THRESHOLD        0.75f
#define APP_TFLM_BLAZEFACE_NMS_IOU_THRESHOLD      0.30f
#define APP_TFLM_BLAZEFACE_X_SCALE                128.0f
#define APP_TFLM_BLAZEFACE_Y_SCALE                128.0f
#define APP_TFLM_BLAZEFACE_W_SCALE                128.0f
#define APP_TFLM_BLAZEFACE_H_SCALE                128.0f

/* MobileFaceNet contract and recognition policy. */
#define APP_TFLM_MOBILEFACENET_INPUT_SIZE          112
#define APP_TFLM_MOBILEFACENET_EMBEDDING_DIM       192

/*
 * MobileFaceNet-style RGB normalization:
 *     normalized = pixel / 127.5 - 1.0
 *
 * The embedding is L2-normalized after inference and compared with cosine
 * similarity. Tune the threshold only after collecting validation samples.
 */
#define APP_TFLM_RECOG_SIMILARITY_THRESHOLD        0.60f

/*
 * Alignment geometry used by the model source:
 * crop size = inter-eye distance * 2.5
 * crop center is shifted down from the eye midpoint by 15% of crop size.
 */
#define APP_TFLM_RECOG_EYE_DISTANCE_SCALE          2.5f
#define APP_TFLM_RECOG_EYE_CENTER_DOWN_SHIFT       0.15f

/* Separate database: never overwrite the ESP-DL /sdcard/FACE.DB. */
#define APP_TFLM_RECOG_DATABASE_PATH "/sdcard/TFLMFP32.DB"

/* Existing raw RGB888 enrollment dataset. */
#define APP_FACE_ENROLL_ROOT          "/sdcard/enroll"
#define APP_FACE_ENROLL_WIDTH         320
#define APP_FACE_ENROLL_HEIGHT        240
