#pragma once

#include "driver/gpio.h"

/* Camera and display buffers */
#define APP_CAMERA_BUFFER_COUNT             2
#define APP_DISPLAY_BUFFER_COUNT            2

/* Face-processing configuration */
#define APP_FACE_DETECT_INTERVAL_FRAMES     5
#define APP_FACE_RECOG_INTERVAL_FRAMES      20
#define APP_MAX_FACE_BOXES                  5
#define APP_FACE_BOX_HOLD_MISSES            3
#define APP_FACE_RECOG_MIN_SCORE            0.85f

/* Cache synchronization */
#define APP_SYNC_CACHE_AROUND_OVERLAY       1

/* Deep-sleep configuration */
#define APP_DEEP_SLEEP_TIMEOUT_MS           30000
#define APP_DEEP_SLEEP_BUTTON_GPIO          GPIO_NUM_3
#define APP_DEEP_SLEEP_BUTTON_POLL_MS       5
#define APP_DEEP_SLEEP_BUTTON_DEBOUNCE_MS   25
#define APP_DEEP_SLEEP_BUTTON_PRIORITY      8

/* Face-database configuration */
#define APP_BUILD_FACE_DB_FROM_SD           0
