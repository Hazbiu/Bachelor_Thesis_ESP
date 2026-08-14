#pragma once

#include "driver/gpio.h"

/* Camera and display buffers */
#define APP_CAMERA_BUFFER_COUNT             2
#define APP_DISPLAY_BUFFER_COUNT            2

/* Face-processing configuration */
#define APP_FACE_DETECT_INTERVAL_FRAMES             2U
#define APP_FACE_DETECT_IDLE_INTERVAL_FRAMES        8U
#define APP_FACE_RECOG_INTERVAL_FRAMES              10U
#define APP_MAX_FACE_BOXES                          5
#define APP_FACE_BOX_HOLD_MISSES                    5
#define APP_FACE_RECOG_MIN_SCORE                    0.70f
#define APP_FACE_BOX_THICKNESS                      3
#define APP_FACE_LABEL_FONT_SCALE                   3

/*
 * Adaptive active-power policy.
 *
 * ACTIVE (0..5 seconds without a face):
 *   - camera and MIPI-DSI display are active;
 *   - CPU is fixed at 360 MHz;
 *   - detection runs every 2 camera frames.
 *
 * IDLE-SCAN (5..15 seconds without a face):
 *   - LVGL, backlight and MIPI-DSI are suspended first;
 *   - the camera remains active;
 *   - CPU is then fixed at 180 MHz;
 *   - detection runs every 8 camera frames;
 *   - recognition is not run until ACTIVE mode has been restored.
 *
 * A face or accepted touchscreen event restores 360 MHz before the display is
 * reinitialized. At 15 seconds the existing coordinated Light-sleep path stops
 * the camera. At 30 seconds the system enters Deep-sleep.
 */
#define APP_CPU_ACTIVE_FREQ_MHZ                     360
#define APP_CPU_IDLE_SCAN_FREQ_MHZ                  180
#define APP_CPU_IDLE_SCAN_AFTER_MS                  5000U

/* Cache synchronization */
#define APP_SYNC_CACHE_AROUND_OVERLAY               1

/* Light-sleep configuration */
#define APP_LIGHT_SLEEP_WAKE_GPIO                   GPIO_NUM_3
#define APP_LIGHT_SLEEP_TIMEOUT_MS                  15000U
#define APP_LIGHT_SLEEP_TOUCH_POLL_MS               250U
#define APP_LIGHT_SLEEP_BUTTON_POLL_MS              5U
#define APP_LIGHT_SLEEP_BUTTON_DEBOUNCE_MS          25U

/* Deep-sleep configuration */
#define APP_DEEP_SLEEP_TIMEOUT_MS                   30000U
#define APP_DEEP_SLEEP_INACTIVITY_POLL_MS           100U
#define APP_DEEP_SLEEP_BUTTON_GPIO                  GPIO_NUM_3
#define APP_DEEP_SLEEP_BUTTON_POLL_MS               5U
#define APP_DEEP_SLEEP_BUTTON_DEBOUNCE_MS           25U
#define APP_DEEP_SLEEP_BUTTON_PRIORITY              8

#if APP_CPU_IDLE_SCAN_AFTER_MS >= APP_LIGHT_SLEEP_TIMEOUT_MS
#error "Idle-scan must begin before Light-sleep"
#endif
#if APP_LIGHT_SLEEP_TIMEOUT_MS >= APP_DEEP_SLEEP_TIMEOUT_MS
#error "Light-sleep must begin before Deep-sleep"
#endif

#if APP_FACE_DETECT_INTERVAL_FRAMES == 0 || \
    APP_FACE_DETECT_IDLE_INTERVAL_FRAMES == 0
#error "Every face-detection interval must be greater than zero"
#endif
