#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    SemaphoreHandle_t *display_mode_mutex;
    lv_display_t **display;
    int *video_fd;
    uint8_t *display_buffer_index;
    bool *dummy_draw_enabled;
    esp_err_t (*release_face_boost)(const char *reason);
} authentication_flow_context_t;

void authentication_flow_initialize(
    const authentication_flow_context_t *context);

void authentication_flow_on_recognition_started(void);

bool authentication_flow_request_pin(
    const char *recognized_name);

void authentication_flow_on_recognition_not_authenticated(void);

bool authentication_flow_launch_pin_transition(void);

#ifdef __cplusplus
}
#endif
