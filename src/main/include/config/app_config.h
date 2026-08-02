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

/* Cache synchronization */
#define APP_SYNC_CACHE_AROUND_OVERLAY       1

/* Deep-sleep configuration */
#define APP_DEEP_SLEEP_TIMEOUT_MS           30000
#define APP_DEEP_SLEEP_BUTTON_GPIO          GPIO_NUM_3
#define APP_DEEP_SLEEP_BUTTON_POLL_MS       5
#define APP_DEEP_SLEEP_BUTTON_DEBOUNCE_MS   25
#define APP_DEEP_SLEEP_BUTTON_PRIORITY      8
