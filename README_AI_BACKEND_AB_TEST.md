# ESP32-P4 dual AI backend A/B update

This update keeps the existing application architecture intact and makes the
complete face-AI stack selectable at compile time.

## Backends

### 1. ESP-DL reference

- Detector: existing `HumanFaceDetect::MSRMNP_S8_V1`
- Recognizer: existing `HumanFaceRecognizer` / MFN S8
- Database: `/sdcard/FACE.DB`

### 2. TFLM-FP32 comparison

- Detector model: `/sdcard/models/face_detector_fp32.tflite`
  - input `[1,128,128,3]` float32
  - output `[1,896,16]` regressors
  - output `[1,896,1]` classification logits
  - BlazeFace 896-anchor decoding + weighted NMS
- Recognizer model: `/sdcard/models/face_embedding_fp32.tflite`
  - input `[1,112,112,3]` float32
  - output `[1,192]` float32
  - eye-based affine face alignment
  - L2-normalized embedding + cosine similarity
- Database: `/sdcard/FACE_TFLM_FP32.DB`

The downloaded models are FP32. This backend is therefore intentionally named
`TFLM-FP32`; it is not claimed to be an INT8 ESP-NN benchmark.

## One-command switching

From the project root:

```bash
chmod +x tools/*.sh
```

Flash ESP-DL:

```bash
./tools/flash_ai_backend.sh espdl /dev/ttyACM0
```

Flash TFLM FP32:

```bash
./tools/flash_ai_backend.sh tflm-fp32 /dev/ttyACM0
```

Build without flashing:

```bash
./tools/build_ai_backend.sh espdl
./tools/build_ai_backend.sh tflm-fp32
```

The selection is written into:

```text
src/main/include/config/ai_backend_selection.h
```

You can also edit it manually.

ESP-DL:

```c
#define APP_AI_BACKEND APP_AI_BACKEND_ESPDL
```

TFLM FP32:

```c
#define APP_AI_BACKEND APP_AI_BACKEND_TFLM_FP32
```

## SD card

The TFLM-FP32 backend requires exactly:

```text
/sdcard/models/face_detector_fp32.tflite
/sdcard/models/face_embedding_fp32.tflite
```

The user's current SD card already has these filenames.

Enrollment remains:

```text
/sdcard/enroll/<person-name>/*.rgb
```

On the first TFLM-FP32 boot, the firmware creates:

```text
/sdcard/FACE_TFLM_FP32.DB
```

Later boots load the cached database unless the model/enrollment fingerprint
changes.

## A/B current measurement

Use exactly the same board, supply voltage, camera scene, enrollment dataset,
CPU policy, display state, detection interval, and Joulescope sampling setup for
both flashes.

Recommended measurement sequence:

1. Flash `espdl`.
2. Allow boot/enrollment work to finish.
3. Record a fixed-duration active no-face trace.
4. Record a fixed-duration face-detection trace.
5. Record repeated recognition/PIN transitions.
6. Save the trace with `ESP-DL` in the filename.
7. Flash `tflm-fp32`.
8. Allow its first database build to finish.
9. Repeat exactly the same measurements.
10. Compare mean current (A or mA), peak current, energy per recognition (J or
    mJ), and recognition latency.

Do not include the first database rebuild in the steady-state inference current
comparison.

The firmware always prints one startup marker:

```text
[AI-BENCH-CONFIG] detector=... recognizer=...
```

Normal per-inference AI logs remain controlled by:

```c
#define APP_LOG_ENABLE_AI 0
```

in `src/main/include/config/log_config.h`.

For current/energy measurements, keep it at `0` so serial logging does not add
avoidable workload. Temporarily set it to `1` when you want timing lines such
as:

```text
[AI-BENCH] backend=TFLM-FP32 model=BlazeFace ...
[AI-BENCH] backend=TFLM-FP32 model=MobileFaceNet ...
```

## Important scientific interpretation

This A/B test compares two complete deployed AI pipelines:

- ESP-DL + MSR/MNP + MFN S8
- TFLM FP32 + BlazeFace + MobileFaceNet

Because the models themselves differ, this is a system-level backend/model
comparison, not a pure runtime-only ESP-DL-versus-ESP-NN comparison.

A future third backend can use truly INT8 TFLite models to measure the
ESP-NN-optimized path separately.
