#pragma once

#include "driver/gpio.h"

/* Camera and display buffers */
#define APP_CAMERA_BUFFER_COUNT             2
#define APP_DISPLAY_BUFFER_COUNT            2

/* Face-processing configuration */
#define APP_FACE_DETECT_INTERVAL_FRAMES     2
#define APP_FACE_RECOG_INTERVAL_FRAMES      10
#define APP_MAX_FACE_BOXES                  5
#define APP_FACE_BOX_HOLD_MISSES            5
#define APP_FACE_RECOG_MIN_SCORE            0.70f
#define APP_FACE_BOX_THICKNESS              3
#define APP_FACE_LABEL_FONT_SCALE           3

/*
 * Active-mode power stages.
 *
 * 0..5 s without activity (PERFORMANCE):
 *   - CPU policy is 90..360 MHz
 *   - normal detection interval
 *   - AI explicitly requests 360 MHz during inference
 *
 * 5..10 s without activity (BALANCED):
 *   - CPU remains in the safe 90..360 MHz DFS range
 *   - the application no longer forces 360 MHz for no-face detection
 *   - detection runs less often
 *
 * 10..15 s without activity (ECO):
 *   - CPU remains in the safe 90..360 MHz DFS range
 *   - the application AI maximum-frequency lock remains disabled
 *   - detection is throttled further
 *
 * At 15 s app_sleep.c enters coordinated Light-sleep. At 30 s it enters
 * Deep-sleep if no touchscreen, face, PIN or GPIO3 activity resets the timer.
 */
#define APP_CPU_BALANCED_AFTER_MS                5000
#define APP_CPU_ECO_AFTER_MS                    10000
#define APP_FACE_DETECT_BALANCED_INTERVAL_FRAMES    4
#define APP_FACE_DETECT_ECO_INTERVAL_FRAMES         8

/* Cache synchronization */
#define APP_SYNC_CACHE_AROUND_OVERLAY       1

/* Light-sleep configuration */
#define APP_LIGHT_SLEEP_WAKE_GPIO           GPIO_NUM_3
#define APP_LIGHT_SLEEP_TIMEOUT_MS          15000
#define APP_LIGHT_SLEEP_TOUCH_POLL_MS       250
#define APP_LIGHT_SLEEP_BUTTON_POLL_MS      5
#define APP_LIGHT_SLEEP_BUTTON_DEBOUNCE_MS  25

/* Deep-sleep configuration (short 30-second deadline for testing) */
#define APP_DEEP_SLEEP_TIMEOUT_MS           30000
#define APP_DEEP_SLEEP_INACTIVITY_POLL_MS   100
#define APP_DEEP_SLEEP_BUTTON_GPIO          GPIO_NUM_3
#define APP_DEEP_SLEEP_BUTTON_POLL_MS       5
#define APP_DEEP_SLEEP_BUTTON_DEBOUNCE_MS   25
#define APP_DEEP_SLEEP_BUTTON_PRIORITY      8

#if APP_CPU_BALANCED_AFTER_MS >= APP_CPU_ECO_AFTER_MS
#error "Balanced CPU stage must begin before the ECO stage"
#endif

#if APP_CPU_ECO_AFTER_MS >= APP_LIGHT_SLEEP_TIMEOUT_MS
#error "ECO CPU stage must begin before Light-sleep"
#endif

#if APP_FACE_DETECT_INTERVAL_FRAMES == 0 || \
    APP_FACE_DETECT_BALANCED_INTERVAL_FRAMES == 0 || \
    APP_FACE_DETECT_ECO_INTERVAL_FRAMES == 0
#error "Every face-detection interval must be greater than zero"
#endif
